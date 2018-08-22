/*******************************************************************************
 *
 * Difference Bound Matrix domain based on the paper "Exploiting
 * Sparsity in Difference-Bound Matrices" by Gange, Navas, Schachte,
 * Sondergaard, and Stuckey published in SAS'16.

 * A re-engineered implementation of the Difference Bound Matrix
 * domain, which maintains bounds and relations separately.
 *
 * Closure operations based on the paper "Fast and Flexible Difference
 * Constraint Propagation for DPLL(T)" by Cotton and Maler.
 *
 * Author: Graeme Gange (gkgange@unimelb.edu.au)
 *
 * Contributors: Jorge A. Navas (jorge.navas@sri.com)
 ******************************************************************************/

#pragma once

#include <crab/common/types.hpp>
#include <crab/common/debug.hpp>
#include <crab/common/stats.hpp>
#include <crab/domains/graphs/adapt_sgraph.hpp>
#include <crab/domains/graphs/sparse_graph.hpp>
#include <crab/domains/graphs/ht_graph.hpp>
#include <crab/domains/graphs/pt_graph.hpp>
#include <crab/domains/graphs/graph_ops.hpp>
#include <crab/domains/linear_constraints.hpp>
#include <crab/domains/intervals.hpp>
#include <crab/domains/patricia_trees.hpp>
#include <crab/domains/operators_api.hpp>
#include <crab/domains/domain_traits.hpp>

#include <type_traits>

#include <boost/optional.hpp>
#include <boost/unordered_set.hpp>
#include <boost/container/flat_map.hpp>

#define CLOSE_BOUNDS_INLINE
// #define CHECK_POTENTIAL
//#define SDBM_NO_NORMALIZE

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"

namespace crab {

  namespace domains {

     namespace SDBM_impl {
       // translate from Number to dbm val_t type
       template<typename Number, typename Wt>
       class NtoV {
       public:
         static Wt ntov(const Number& n) { 
           return (Wt) n;
         }
       };

       // All of these representations are implementations of a
       // sparse weighted graph. They differ on the datastructures
       // used to store successors and predecessors
       enum GraphRep { 
         // sparse-map and sparse-sets
         ss = 1,        
         // adaptive sparse-map and sparse-sets
         adapt_ss = 2,  
         // patricia tree-maps and patricia tree-sets
         pt = 3,           
         // hash table and hash sets
         ht = 4
       };          

       template<typename Number, GraphRep Graph = GraphRep::adapt_ss>
       class DefaultParams {
       public:
         enum { chrome_dijkstra = 1 };
         enum { widen_restabilize = 1 };
         enum { special_assign = 1 };

	 // use long as graph weights
         typedef long Wt; 

         typedef typename std::conditional< 
           (Graph == ss), 
           SparseWtGraph<Wt>,
           typename std::conditional< 
             (Graph == adapt_ss), 
             AdaptGraph<Wt>,
             typename std::conditional< 
               (Graph == pt), 
               PtGraph<Wt>, 
               HtGraph<Wt> 
               >::type 
             >::type 
           >::type graph_t;
       };

       template<typename Number, GraphRep Graph = GraphRep::adapt_ss>
       class SimpleParams {
       public:
         enum { chrome_dijkstra = 0 };
         enum { widen_restabilize = 0 };
         enum { special_assign = 0 };

         typedef long Wt;

         typedef typename std::conditional< 
           (Graph == ss), 
           SparseWtGraph<Wt>,
           typename std::conditional< 
             (Graph == adapt_ss), 
             AdaptGraph<Wt>,
             typename std::conditional< 
               (Graph == pt), 
               PtGraph<Wt>, 
               HtGraph<Wt> 
               >::type 
             >::type 
           >::type graph_t;
       };

       // We don't use GraphRep::adapt_ss because having problems
       // realloc'ed Number objects.
       template<typename Number, GraphRep Graph = GraphRep::ss>
       class BigNumDefaultParams {
       public:
         enum { chrome_dijkstra = 1 };
         enum { widen_restabilize = 1 };
         enum { special_assign = 1 };

	 // Use Number as graph weights
         typedef Number Wt;

         typedef typename std::conditional< 
           (Graph == ss), 
           SparseWtGraph<Wt>,
           typename std::conditional< 
             (Graph == adapt_ss), 
             AdaptGraph<Wt>,
             typename std::conditional< 
               (Graph == pt), 
               PtGraph<Wt>, 
               HtGraph<Wt> 
               >::type 
             >::type 
           >::type graph_t;
       };
     }; // end namespace SDBM_impl


    template<class Number, class VariableName, class Params = SDBM_impl::DefaultParams<Number> >
    class SplitDBM_ :
      public abstract_domain<Number, VariableName,
			     SplitDBM_<Number,VariableName,Params> > {

      typedef SplitDBM_<Number, VariableName, Params> DBM_t;
      typedef abstract_domain<Number, VariableName, DBM_t> abstract_domain_t;
      
     public:
      using typename abstract_domain_t::linear_expression_t;
      using typename abstract_domain_t::linear_constraint_t;
      using typename abstract_domain_t::linear_constraint_system_t;
      using typename abstract_domain_t::variable_t;
      using typename abstract_domain_t::number_t;
      using typename abstract_domain_t::varname_t;
      using typename abstract_domain_t::variable_vector_t;
      
      typedef typename linear_constraint_t::kind_t constraint_kind_t;
      typedef interval<Number>  interval_t;

     private:
      typedef bound<Number>  bound_t;
      typedef typename Params::Wt Wt;
      typedef typename Params::graph_t graph_t;
      typedef SDBM_impl::NtoV<Number, Wt> ntov;
      typedef typename graph_t::vert_id vert_id;
      typedef boost::container::flat_map<variable_t, vert_id> vert_map_t;
      typedef typename vert_map_t::value_type vmap_elt_t;
      typedef std::vector< boost::optional<variable_t> > rev_map_t;
      typedef GraphOps<graph_t> GrOps;
      typedef GraphPerm<graph_t> GrPerm;
      typedef typename GrOps::edge_vector edge_vector;
      // < <x, y>, k> == x - y <= k.
      typedef std::pair< std::pair<variable_t, variable_t>, Wt > diffcst_t;
      typedef boost::unordered_set<vert_id> vert_set_t;

      protected:
        
      //================
      // Domain data
      //================
      // GKG: ranges are now maintained in the graph
      vert_map_t vert_map; // Mapping from variables to vertices
      rev_map_t rev_map;
      graph_t g; // The underlying relation graph
      std::vector<Wt> potential; // Stored potential for the vertex
      vert_set_t unstable;
      bool _is_bottom;


      void set_to_bottom() {
        vert_map.clear();
        rev_map.clear();
        g.clear();
        potential.clear();
        unstable.clear();
        _is_bottom = true;
      }

      boost::optional<std::pair<vert_id,std::pair<vert_id, Wt> > >
      diffcst_of_leq(linear_constraint_t cst) {

        assert (cst.size() > 0);
        assert (cst.is_inequality());

        std::vector<std::pair<vert_id,std::pair<vert_id, Wt> > > diffcsts;

        typename linear_expression_t::iterator it1 = cst.begin();
        typename linear_expression_t::iterator it2 = ++cst.begin();
        vert_id i, j;
        
        if (cst.size() == 1 && it1->first == 1) {
          i = get_vert(it1->second);
          j = 0;
        } else if (cst.size() == 1 && it1->first == -1) {
          i = 0;
          j = get_vert(it1->second);
        } else if (cst.size() == 2 && it1->first == 1 && it2->first == -1) {
          i = get_vert(it1->second);
          j = get_vert(it2->second);
        } else if (cst.size() == 2 && it1->first == -1 && it2->first == 1) {
          i = get_vert(it2->second);
          j = get_vert(it1->second);
        } else { 
          // the constraint cannot be expressed as a difference constraint
          return boost::none;
        }
        return std::make_pair(j, std::make_pair(i, Wt(cst.constant())));
      }

      class Wt_max {
      public:
       Wt_max() { } 
       Wt apply(const Wt& x, const Wt& y) { return std::max(x, y); }
       bool default_is_absorbing() { return true; }
      };

      class Wt_min {
      public:
        Wt_min() { }
        Wt apply(const Wt& x, const Wt& y) { return std::min(x, y); }
        bool default_is_absorbing() { return false; }
      };

      vert_id get_vert(variable_t v) {
        auto it = vert_map.find(v);
        if(it != vert_map.end())
          return (*it).second;

        vert_id vert(g.new_vertex());
        vert_map.insert(vmap_elt_t(v, vert)); 
        // Initialize 
        assert(vert <= rev_map.size());
        if(vert < rev_map.size())
        {
          potential[vert] = Wt(0);
          rev_map[vert] = v;
        } else {
          potential.push_back(Wt(0));
          rev_map.push_back(v);
        }
        vert_map.insert(vmap_elt_t(v, vert));

        assert(vert != 0);

        return vert;
      }

      vert_id get_vert(graph_t& g, vert_map_t& vmap, rev_map_t& rmap,
		       std::vector<Wt>& pot, variable_t v) {
        auto it = vmap.find(v);
        if(it != vmap.end())
          return (*it).second;

        vert_id vert(g.new_vertex());
        vmap.insert(vmap_elt_t(v, vert)); 
        // Initialize 
        assert(vert <= rmap.size());
        if(vert < rmap.size())
        {
          pot[vert] = Wt(0);
          rmap[vert] = v;
        } else {
          pot.push_back(Wt(0));
          rmap.push_back(v);
        }
        vmap.insert(vmap_elt_t(v, vert));

        return vert;
      }

      template<class G, class P>
      inline bool check_potential(G& g, P& p)
      {
        #ifdef CHECK_POTENTIAL
        for(vert_id v : g.verts())
        {
          for(vert_id d : g.succs(v))
          {
            if(p[v] + g.edge_val(v, d) - p[d] < Wt(0))
            {
              assert(0 && "Invalid potential.");
              return false;
            }
          }
        }
        #endif
        return true;
      }

      class vert_set_wrap_t {
      public:
        vert_set_wrap_t(const vert_set_t& _vs)
          : vs(_vs)
        { }

        bool operator[](vert_id v) const {
          return vs.find(v) != vs.end();
        }
        const vert_set_t& vs;
      };

      // Evaluate the potential value of a variable.
      Wt pot_value(variable_t v) {
        auto it = vert_map.find(v); 
        if(it != vert_map.end())
          return potential[(*it).second];
        return ((Wt) 0);
      }

      Wt pot_value(variable_t v, std::vector<Wt>& potential) {
        auto it = vert_map.find(v); 
        if(it != vert_map.end())
          return potential[(*it).second];
        return ((Wt) 0);
      }

      // Evaluate an expression under the chosen potentials
      Wt eval_expression(linear_expression_t e) {
        Wt v(ntov::ntov(e.constant())); 
        for(auto p : e) {
          v += (pot_value(p.second) - potential[0])*(ntov::ntov(p.first));
        }
        return v;
      }
      
      interval_t eval_interval(linear_expression_t e) {
        interval_t r = e.constant();
        for (auto p : e)
          r += p.first * operator[](p.second);
        return r;
      }

      interval_t compute_residual(linear_expression_t e, variable_t pivot) {
	interval_t residual(-e.constant());
	for (typename linear_expression_t::iterator it = e.begin(); it != e.end(); ++it) {
	  variable_t v = it->second;
	  if (v.index() != pivot.index()) {
	    residual = residual - (interval_t (it->first) * this->operator[](v));
	  }
	}
	return residual;
      }
      
      // Turn an assignment into a set of difference constraints.
      void diffcsts_of_assign(variable_t x, linear_expression_t exp,
			      std::vector<std::pair<variable_t, Wt> >& lb,
			      std::vector<std::pair<variable_t,Wt> >& ub) {
        {
          // Process upper bounds.
          boost::optional<variable_t> unbounded_ubvar;
          Wt exp_ub(ntov::ntov(exp.constant()));
          std::vector< std::pair<variable_t, Wt> > ub_terms;
          for(auto p : exp)
          {
            Wt coeff(ntov::ntov(p.first));
            if(p.first < Wt(0))
            {
              // Can't do anything with negative coefficients.
              bound_t y_lb = operator[](p.second).lb();
              if(y_lb.is_infinite())
                goto assign_ub_finish;
              exp_ub += ntov::ntov(*(y_lb.number()))*coeff;
            } else {
              variable_t y(p.second);
              bound_t y_ub = operator[](y).ub(); 
              if(y_ub.is_infinite())
              {
                if(unbounded_ubvar || coeff != Wt(1))
                  goto assign_ub_finish;
                unbounded_ubvar = y;
              } else {
                Wt ymax(ntov::ntov(*(y_ub.number())));
                exp_ub += ymax*coeff;
                ub_terms.push_back(std::make_pair(y, ymax));
              }
            }
          }

          if(unbounded_ubvar)
          {
            // There is exactly one unbounded variable. 
            ub.push_back(std::make_pair(*unbounded_ubvar, exp_ub));
          } else {
            for(auto p : ub_terms)
            {
              ub.push_back(std::make_pair(p.first, exp_ub - p.second));
            }
          }
        }
      assign_ub_finish:

        {
          boost::optional<variable_t> unbounded_lbvar;
          Wt exp_lb(ntov::ntov(exp.constant()));
          std::vector< std::pair<variable_t, Wt> > lb_terms;
          for(auto p : exp)
          {
            Wt coeff(ntov::ntov(p.first));
            if(p.first < Wt(0))
            {
              // Again, can't do anything with negative coefficients.
              bound_t y_ub = operator[](p.second).ub();
              if(y_ub.is_infinite())
                goto assign_lb_finish;
              exp_lb += (ntov::ntov(*(y_ub.number())))*coeff;
            } else {
              variable_t y(p.second);
              bound_t y_lb = operator[](y).lb(); 
              if(y_lb.is_infinite())
              {
                if(unbounded_lbvar || coeff != Wt(1))
                  goto assign_lb_finish;
                unbounded_lbvar = y;
              } else {
                Wt ymin(ntov::ntov(*(y_lb.number())));
                exp_lb += ymin*coeff;
                lb_terms.push_back(std::make_pair(y, ymin));
              }
            }
          }

          if(unbounded_lbvar)
          {
            lb.push_back(std::make_pair(*unbounded_lbvar, exp_lb));
          } else {
            for(auto p : lb_terms)
            {
              lb.push_back(std::make_pair(p.first, exp_lb - p.second));
            }
          }
        }
      assign_lb_finish:
        return;
      }
   
      // GKG: I suspect there're some sign/bound direction errors in the 
      // following.
      void diffcsts_of_lin_leq(const linear_expression_t& exp, std::vector<diffcst_t>& csts,
			       std::vector<std::pair<variable_t, Wt> >& lbs,
			       std::vector<std::pair<variable_t, Wt> >& ubs) {
        // Process upper bounds.
        Wt unbounded_lbcoeff;
        Wt unbounded_ubcoeff;
        boost::optional<variable_t> unbounded_lbvar;
        boost::optional<variable_t> unbounded_ubvar;
        Wt exp_ub = - (ntov::ntov(exp.constant()));
        std::vector< std::pair< std::pair<Wt, variable_t>, Wt> > pos_terms;
        std::vector< std::pair< std::pair<Wt, variable_t>, Wt> > neg_terms;
        for(auto p : exp)
        {
          Wt coeff(ntov::ntov(p.first));
          if(coeff > Wt(0))
          {
            variable_t y(p.second);
            bound_t y_lb = operator[](y).lb();
            if(y_lb.is_infinite())
            {
              if(unbounded_lbvar)
                goto diffcst_finish;
              unbounded_lbvar = y;
              unbounded_lbcoeff = coeff;
            } else {
              Wt ymin(ntov::ntov(*(y_lb.number())));
              // Coeff is negative, so it's still add
              exp_ub -= ymin*coeff;
              pos_terms.push_back(std::make_pair(std::make_pair(coeff, y), ymin));
            }
          } else {
            variable_t y(p.second);
            bound_t y_ub = operator[](y).ub(); 
            if(y_ub.is_infinite())
            {
              if(unbounded_ubvar)
                goto diffcst_finish;
              unbounded_ubvar = y;
              unbounded_ubcoeff = -(ntov::ntov(coeff));
            } else {
              Wt ymax(ntov::ntov(*(y_ub.number())));
              exp_ub -= ymax*coeff;
              neg_terms.push_back(std::make_pair(std::make_pair(-coeff, y), ymax));
            }
          }
        }

        if(unbounded_lbvar)
        {
          variable_t x(*unbounded_lbvar);
          if(unbounded_ubvar)
          {
            if(unbounded_lbcoeff != Wt(1) || unbounded_ubcoeff != Wt(1))
              goto diffcst_finish;
            variable_t y(*unbounded_ubvar);
            csts.push_back(std::make_pair(std::make_pair(x, y), exp_ub));
          } else {
            if(unbounded_lbcoeff == Wt(1))
            {
              for(auto p : neg_terms)
                csts.push_back(std::make_pair(std::make_pair(x, p.first.second),
					      exp_ub - p.second));
            }
            // Add bounds for x
            ubs.push_back(std::make_pair(x, exp_ub/unbounded_lbcoeff));
          }
        } else {
          if(unbounded_ubvar)
          {
            variable_t y(*unbounded_ubvar);
            if(unbounded_ubcoeff == Wt(1))
            {
              for(auto p : pos_terms)
                csts.push_back(std::make_pair(std::make_pair(p.first.second, y),
					      exp_ub + p.second));
            }
            // Bounds for y
            lbs.push_back(std::make_pair(y, -exp_ub/unbounded_ubcoeff));
          } else {
            for(auto pl : neg_terms)
              for(auto pu : pos_terms)
                csts.push_back(std::make_pair(std::make_pair(pu.first.second, pl.first.second),
					 exp_ub - pl.second + pu.second));
            for(auto pl : neg_terms)
              lbs.push_back(std::make_pair(pl.first.second, -exp_ub/pl.first.first + pl.second));
            for(auto pu : pos_terms)
              ubs.push_back(std::make_pair(pu.first.second, exp_ub/pu.first.first + pu.second));
          }
        }
    diffcst_finish:
        return;
      }
      

      bool add_linear_leq(const linear_expression_t& exp)
      {
        CRAB_LOG("zones-split",
                 linear_expression_t exp_tmp (exp);
                 crab::outs() << "Adding: "<< exp_tmp << "<= 0" <<"\n");
        std::vector< std::pair<variable_t, Wt> > lbs;
        std::vector< std::pair<variable_t, Wt> > ubs;
        std::vector<diffcst_t> csts;
        diffcsts_of_lin_leq(exp, csts, lbs, ubs);

        assert(check_potential(g, potential));

        Wt_min min_op;
        typename graph_t::mut_val_ref_t w;
        for(auto p : lbs)
        {
          CRAB_LOG("zones-split",
                   crab::outs() << p.first<< ">="<< p.second <<"\n");
          variable_t x(p.first);
          vert_id v = get_vert(p.first);
          if(g.lookup(v, 0, &w) && w.get() <= -p.second)
            continue;
          g.set_edge(v, -p.second, 0);
          if(!repair_potential(v, 0))
          {
            set_to_bottom();
            return false;
          }
          assert(check_potential(g, potential));
          // Compute other updated bounds
          #ifdef CLOSE_BOUNDS_INLINE
          for(auto e : g.e_preds(v))
          {
            if(e.vert == 0)
              continue;
            g.update_edge(e.vert, e.val - p.second, 0, min_op);

	    if(!repair_potential(e.vert, 0))
	    {
	      set_to_bottom();
	      return false;
	    }
	    assert(check_potential(g, potential));
          }
          #endif
        }
        for(auto p : ubs)
        {
          CRAB_LOG("zones-split",
                   crab::outs() << p.first<< "<="<< p.second <<"\n");
          variable_t x(p.first);
          vert_id v = get_vert(p.first);
          if(g.lookup(0, v, &w) && w.get() <= p.second)
            continue;
          g.set_edge(0, p.second, v);
          if(!repair_potential(0, v))
          {
            set_to_bottom();
            return false;
          }
          assert(check_potential(g, potential));

          #ifdef CLOSE_BOUNDS_INLINE
          for(auto e : g.e_succs(v))
          {
            if(e.vert == 0)
              continue;
            g.update_edge(0, e.val + p.second, e.vert, min_op);
	    if(!repair_potential(0, e.vert))
	    {
	      set_to_bottom();
	      return false;
	    }
	    assert(check_potential(g, potential));
          }
          #endif
        }

        for(auto diff : csts)
        {
          CRAB_LOG("zones-split",
                   crab::outs() << diff.first.first<< "-"<< diff.first.second<< "<="
		                << diff.second <<"\n");

          vert_id src = get_vert(diff.first.second);
          vert_id dest = get_vert(diff.first.first);
          g.update_edge(src, diff.second, dest, min_op);
          if(!repair_potential(src, dest))
          {
            set_to_bottom();
            return false;
          }
          assert(check_potential(g, potential));
          
          close_over_edge(src, dest);
          assert(check_potential(g, potential));
        }
        // Collect bounds
        // GKG: Now done in close_over_edge

        #ifndef CLOSE_BOUNDS_INLINE
        edge_vector delta;
        GrOps::close_after_assign(g, potential, 0, delta);
        GrOps::apply_delta(g, delta);
        #endif

        assert(check_potential(g, potential));
        // CRAB_WARN("SplitDBM::add_linear_leq not yet implemented.");
        return true;  
      }

      // x != n
      void add_univar_disequation(variable_t x, number_t n) {
	interval_t i = get_interval(x);
	interval_t new_i =
	  linear_interval_solver_impl::trim_interval<interval_t>(i, interval_t(n));
	if (new_i.is_bottom()) {
	  set_to_bottom();
	} else if (!new_i.is_top() && (new_i <= i)) {
	  vert_id v = get_vert(x);
	  typename graph_t::mut_val_ref_t w;
	  if(new_i.lb().is_finite()) {
	    // strenghten lb
	    Wt lb_val = ntov::ntov(-(*(new_i.lb().number())));
	    if(g.lookup(v, 0, &w) && lb_val < w.get()) {
	      g.set_edge(v, lb_val, 0);
	      if(!repair_potential(v, 0)) {
		set_to_bottom();
		return;
	      }
	      assert(check_potential(g, potential));
	    }
	  }
	  if(new_i.ub().is_finite()) {	    
	    // strengthen ub
	    Wt ub_val = ntov::ntov(*(new_i.ub().number()));
	    if(g.lookup(0, v, &w) && (ub_val < w.get())) {
	      g.set_edge(0, ub_val, v);
	      if(!repair_potential(0, v)) {
		set_to_bottom();
		return;
	      }
	      assert(check_potential(g, potential));
	    }
	  }
	}
      } 
      
      void add_disequation(linear_expression_t e) {
	// XXX: similar precision as the interval domain
	
	for (typename linear_expression_t::iterator it = e.begin(); it != e.end(); ++it) {
	  variable_t pivot = it->second;
	  interval_t i = compute_residual(e, pivot) / interval_t(it->first);
	  if (auto k = i.singleton()) {
	    add_univar_disequation(pivot, *k);
	  }
	}
        return;
        /*
        // Can only exploit \sum_i c_i x_i \neq k if:
        // (1) exactly one x_i is unfixed
        // (2) lb(x_i) or ub(x_i) = k - \sum_i' c_i' x_i'
        Wt k = exp.constant();
        auto it = exp.begin();
        for(; it != exp.end(); ++it)
        {
          if(!var_is_fixed((*it).second)) 
            break;
          k -= (*it).first*get_value((*it).second);
        }

        // All variables are fixed
        if(it == exp.end())
        {
          if(k == Wt(0))
            set_to_bottom();
          return;
        }

        // Found one unfixed variable; collect the rest.
        Wt ucoeff = (*it).first;
        VariableName uvar((*it).second;
        interval_t u_int = get_interval(ranges, uvar);
        // We need at least one side of u to be finite.
        if(u_int.lb().is_infinite() && u_int.ub().is_infinite())
          return;

        for(++it; it != exp.end(); ++it)
        {
          // Two unfixed variables; nothing we can do.
          if(!var_is_fixed((*it).second))
            return;
          k -= (*it).first*get_value((*it).second);
        }
        */
      }

      interval_t get_interval(variable_t x) {
        return get_interval(vert_map, g, x);
      }

      interval_t get_interval(vert_map_t& m, graph_t& r, variable_t x) {
        auto it = m.find(x);
        if(it == m.end())
        {
          return interval_t::top();
        }
        vert_id v = (*it).second;
        interval_t x_out = interval_t(
            r.elem(v, 0) ? -Number(r.edge_val(v, 0)) : bound_t::minus_infinity(),
            r.elem(0, v) ? Number(r.edge_val(0, v)) : bound_t::plus_infinity());
        return x_out;
        /*
        boost::optional< interval_t > v = r.lookup(x);
        if(v)
          return *v;
        else
          return interval_t::top();
	*/
      }

      // Resore potential after an edge addition
      bool repair_potential(vert_id src, vert_id dest)
      {
        return GrOps::repair_potential(g, potential, src, dest);
      }

      // Restore closure after a single edge addition
      void close_over_edge(vert_id ii, vert_id jj)
      {
        Wt_min min_op;

        assert(ii != 0 && jj != 0);
        SubGraph<graph_t> g_excl(g, 0);

        Wt c = g_excl.edge_val(ii,jj);

        typename graph_t::mut_val_ref_t w;
        #ifdef CLOSE_BOUNDS_INLINE
        if(g.lookup(0, ii, &w))
          g.update_edge(0, w.get() + c, jj, min_op);
        if(g.lookup(jj, 0, &w))
          g.update_edge(ii, w.get() + c, 0, min_op);
        #endif

        // There may be a cheaper way to do this.
        // GKG: Now implemented.
        std::vector<std::pair<vert_id, Wt> > src_dec;
        for(auto edge : g_excl.e_preds(ii))
        {
          vert_id se = edge.vert;
          Wt wt_sij = edge.val + c;

          assert(g_excl.succs(se).begin() != g_excl.succs(se).end());
          if(se != jj)
          {
            if(g_excl.lookup(se, jj, &w))
            {
              if(w.get() <= wt_sij)
                continue;

              w = wt_sij;
              // g_excl.set_edge(se, wt_sij, jj);
            } else {
              g_excl.add_edge(se, wt_sij, jj);
            }
            src_dec.push_back(std::make_pair(se, edge.val));  
            #ifdef CLOSE_BOUNDS_INLINE
            if(g.lookup(0, se, &w))
              g.update_edge(0, w.get() + wt_sij, jj, min_op);
            if(g.lookup(jj, 0, &w))
              g.update_edge(se, w.get() + wt_sij, 0, min_op);
            #endif

	    /*
            for(auto edge : g_excl.e_succs(jj))
            {
              vert_id de = edge.vert;
              if(se != de)
              {
                Wt wt_sijd = wt_sij + edge.val;
                if(g_excl.lookup(se, de, &w))
                {
                  if((*w) <= wt_sijd)
                    continue;
                  (*w) = wt_sijd;
                } else {
                  g_excl.add_edge(se, wt_sijd, de);
                }
                #ifdef CLOSE_BOUNDS_INLINE
                if(g.lookup(0, se, &w))
                  g.update_edge(0, (*w) + wt_sijd, de, min_op);
                if(g.lookup(de, 0, &w))
                  g.update_edge(se, (*w) + wt_sijd, 0, min_op);
                #endif
              }
            }
            */
          }
        }

        std::vector<std::pair<vert_id, Wt> > dest_dec;
        for(auto edge : g_excl.e_succs(jj))
        {
          vert_id de = edge.vert;
          Wt wt_ijd = edge.val + c;
          if(de != ii)
          {
            if(g_excl.lookup(ii, de, &w))
            {
              if(w.get() <= wt_ijd)
                continue;
              w = wt_ijd;
            } else {
              g_excl.add_edge(ii, wt_ijd, de);
            }
            dest_dec.push_back(std::make_pair(de, edge.val));
            #ifdef CLOSE_BOUNDS_INLINE
            if(g.lookup(0,  ii, &w))
              g.update_edge(0, w.get() + wt_ijd, de, min_op);
            if(g.lookup(de, 0, &w))
              g.update_edge(ii, w.get() + wt_ijd, 0, min_op);
            #endif
          }
        }

        for(auto s_p : src_dec)
        {
          vert_id se = s_p.first;
          Wt wt_sij = c + s_p.second;
          for(auto d_p : dest_dec)
          {
            vert_id de = d_p.first;
            Wt wt_sijd = wt_sij + d_p.second; 
            if(g.lookup(se, de, &w))
            {
              if(w.get() <= wt_sijd)
                continue;
              w = wt_sijd;
            } else {
              g.add_edge(se, wt_sijd, de);
            }
            #ifdef CLOSE_BOUNDS_INLINE
            if(g.lookup(0, se, &w))
              g.update_edge(0, w.get() + wt_sijd, de, min_op);
            if(g.lookup(de, 0, &w))
              g.update_edge(se, w.get() + wt_sijd, 0, min_op);
            #endif
          }
        }

        // Closure is now updated.
      }
    
      // Restore closure after a variable assignment
      // Assumption: x = f(y_1, ..., y_n) cannot induce non-trivial
      // relations between (y_i, y_j)
      /*
      bool close_after_assign(vert_id v)
      {
        // Run Dijkstra's forward to collect successors of v,
        // and backward to collect predecessors
        edge_vector delta; 
        if(!GrOps::close_after_assign(g, potential, v, delta))
          return false;
        GrOps::apply_delta(g, delta);
        return true; 
      }

      bool closure(void)
      {
        // Full Johnson-style all-pairs shortest path
        CRAB_ERROR("SparseWtGraph::closure not yet implemented."); 
      }
      */


      template<typename G>
      bool is_eq (vert_id u, vert_id v, G& g) {
        // pre: rev_map[u] and rev_map[v]
        if (g.elem (u, v) && g.elem (v, u)) {
          return (g.edge_val(u, v) == g.edge_val(v, u));
        } else {
          return false;
        }
      }

      
   public:
      
      SplitDBM_(bool is_bottom = false): _is_bottom(is_bottom)
      {
        g.growTo(1);  // Allocate the zero vector
        potential.push_back(Wt(0));
        rev_map.push_back(boost::none);
      }

      // FIXME: Rewrite to avoid copying if o is _|_
      SplitDBM_(const DBM_t& o)
        : vert_map(o.vert_map),
          rev_map(o.rev_map),
          g(o.g),
          potential(o.potential),
          unstable(o.unstable),
          _is_bottom(false)
      {
        crab::CrabStats::count (getDomainName() + ".count.copy");
        crab::ScopedCrabStats __st__(getDomainName() + ".copy");

        if(o._is_bottom)
          set_to_bottom();

        if(!_is_bottom)
          assert(g.size() > 0);
      }

      SplitDBM_(DBM_t&& o)
        : vert_map(std::move(o.vert_map)), rev_map(std::move(o.rev_map)),
          g(std::move(o.g)), potential(std::move(o.potential)),
          unstable(std::move(o.unstable)),
          _is_bottom(o._is_bottom)
      { }

      // We should probably use the magical rvalue ownership semantics stuff.
      SplitDBM_(vert_map_t& _vert_map, rev_map_t& _rev_map, graph_t& _g,
		std::vector<Wt>& _potential,
        vert_set_t& _unstable)
        : vert_map(_vert_map), rev_map(_rev_map), g(_g),
	  potential(_potential), unstable(_unstable), _is_bottom(false)
      {
        CRAB_WARN("Non-moving constructor.");
        assert(g.size() > 0);
      }
      
      SplitDBM_(vert_map_t&& _vert_map, rev_map_t&& _rev_map, graph_t&& _g,
		std::vector<Wt>&& _potential, vert_set_t&& _unstable)
        : vert_map(std::move(_vert_map)), rev_map(std::move(_rev_map)), g(std::move(_g)),
	  potential(std::move(_potential)), unstable(std::move(_unstable)), _is_bottom(false)
      { assert(g.size() > 0); }


      SplitDBM_& operator=(const SplitDBM_& o)
      {
        crab::CrabStats::count (getDomainName() + ".count.copy");
        crab::ScopedCrabStats __st__(getDomainName() + ".copy");

        if(this != &o)
        {
          if(o._is_bottom)
            set_to_bottom();
          else {
            _is_bottom = false;
            vert_map = o.vert_map;
            rev_map = o.rev_map;
            g = o.g;
            potential = o.potential;
            unstable = o.unstable;
            assert(g.size() > 0);
          }
        }
        return *this;
      }

      SplitDBM_& operator=(SplitDBM_&& o)
      {
        if(o._is_bottom) {
          set_to_bottom();
        } else {
          _is_bottom = false;
          vert_map = std::move(o.vert_map);
          rev_map = std::move(o.rev_map);
          g = std::move(o.g);
          potential = std::move(o.potential);
          unstable = std::move(o.unstable);
        }
        return *this;
      }
             
      static DBM_t top() { return SplitDBM_(false); }
    
      static DBM_t bottom() { return SplitDBM_(true); }
    
      bool is_bottom() const {
        return _is_bottom;
      }
    
      bool is_top() {
        if(_is_bottom)
          return false;
        return g.is_empty();
      }
    
      bool operator<=(DBM_t& o)  {
        crab::CrabStats::count (getDomainName() + ".count.leq");
        crab::ScopedCrabStats __st__(getDomainName() + ".leq");

        // cover all trivial cases to avoid allocating a dbm matrix
        if (is_bottom()) 
          return true;
        else if(o.is_bottom())
          return false;
        else if (o.is_top ())
          return true;
        else if (is_top ())
          return false;
        else {
          normalize();

          // CRAB_LOG("zones-split", crab::outs() << "operator<=: "<< *this<< "<=?"<< o <<"\n");

          if(vert_map.size() < o.vert_map.size()) 
            return false;

          typename graph_t::mut_val_ref_t wx; typename graph_t::mut_val_ref_t wy;

          // Set up a mapping from o to this.
          std::vector<unsigned int> vert_renaming(o.g.size(),-1);
          vert_renaming[0] = 0;
          for(auto p : o.vert_map)
          {
            if(o.g.succs(p.second).size() == 0 && o.g.preds(p.second).size() == 0)
              continue;

            auto it = vert_map.find(p.first);
            // We can't have this <= o if we're missing some
            // vertex.
            if(it == vert_map.end()) 
              return false;
            vert_renaming[p.second] = (*it).second;
            // vert_renaming[(*it).second] = p.second;
          }

          assert(g.size() > 0);
	  // GrPerm g_perm(vert_renaming, g);

          for(vert_id ox : o.g.verts())
          {
            if(o.g.succs(ox).size() == 0)
              continue;

            assert(vert_renaming[ox] != -1);
            vert_id x = vert_renaming[ox];
            for(auto edge : o.g.e_succs(ox))
            {
              vert_id oy = edge.vert;
              assert(vert_renaming[oy] != -1);
              vert_id y = vert_renaming[oy];
              Wt ow = edge.val;

              if(g.lookup(x, y, &wx) && (wx.get() <= ow))
                continue;

              if(!g.lookup(x, 0, &wx) || !g.lookup(0, y, &wy)) 
                return false;
              if(!(wx.get() + wy.get() <= ow)) 
                return false;
              
            }
          }
          return true;
        }
      }
      
      // FIXME: can be done more efficient
      void operator|=(DBM_t o) {
        *this = *this | o;
      }

      DBM_t operator|(DBM_t& o) {
        crab::CrabStats::count (getDomainName() + ".count.join");
        crab::ScopedCrabStats __st__(getDomainName() + ".join");

        if (is_bottom() || o.is_top ())
          return o;
        else if (is_top () || o.is_bottom())
          return *this;
        else {
          CRAB_LOG ("zones-split",
                    crab::outs() << "Before join:\n"<<"DBM 1\n"<<*this<<"\n"<<"DBM 2\n"
		                 << o <<"\n");

          normalize();
          o.normalize();

          assert(check_potential(g, potential));
          assert(check_potential(o.g, o.potential));

          // Figure out the common renaming, initializing the
          // resulting potentials as we go.
          std::vector<vert_id> perm_x;
          std::vector<vert_id> perm_y;
          std::vector<variable_t> perm_inv;

          std::vector<Wt> pot_rx;
          std::vector<Wt> pot_ry;
          vert_map_t out_vmap;
          rev_map_t out_revmap;
          // Add the zero vertex
          assert(potential.size() > 0);
          pot_rx.push_back(0);
          pot_ry.push_back(0);
          perm_x.push_back(0);
          perm_y.push_back(0);
          out_revmap.push_back(boost::none);

          for(auto p : vert_map)
          {
            auto it = o.vert_map.find(p.first); 
            // Variable exists in both
            if(it != o.vert_map.end())
            {
              out_vmap.insert(vmap_elt_t(p.first, perm_x.size()));
              out_revmap.push_back(p.first);

              pot_rx.push_back(potential[p.second] - potential[0]);
              // XXX JNL: check this out
              //pot_ry.push_back(o.potential[p.second] - o.potential[0]);
              pot_ry.push_back(o.potential[(*it).second] - o.potential[0]);
              perm_inv.push_back(p.first);
              perm_x.push_back(p.second);
              perm_y.push_back((*it).second);
            }
          }
          unsigned int sz = perm_x.size();

          // Build the permuted view of x and y.
          assert(g.size() > 0);
          GrPerm gx(perm_x, g);
          assert(o.g.size() > 0);
          GrPerm gy(perm_y, o.g);

          // Compute the deferred relations
          graph_t g_ix_ry;
          g_ix_ry.growTo(sz);
          SubGraph<GrPerm> gy_excl(gy, 0);
          for(vert_id s : gy_excl.verts())
          {
            for(vert_id d : gy_excl.succs(s))
            {
              typename graph_t::mut_val_ref_t ws; typename graph_t::mut_val_ref_t wd;
              if(gx.lookup(s, 0, &ws) && gx.lookup(0, d, &wd)) {
                g_ix_ry.add_edge(s, ws.get() + wd.get(), d);
	      }
            }
          }
          // Apply the deferred relations, and re-close.
          edge_vector delta;
          bool is_closed;
          graph_t g_rx(GrOps::meet(gx, g_ix_ry, is_closed));
          assert(check_potential(g_rx, pot_rx));
          if(!is_closed)
          {
            SubGraph<graph_t> g_rx_excl(g_rx, 0);
            GrOps::close_after_meet(g_rx_excl, pot_rx, gx, g_ix_ry, delta);
            GrOps::apply_delta(g_rx, delta);
          }

          graph_t g_rx_iy;
          g_rx_iy.growTo(sz);

          SubGraph<GrPerm> gx_excl(gx, 0);
          for(vert_id s : gx_excl.verts())
          {
            for(vert_id d : gx_excl.succs(s))
            {
              typename graph_t::mut_val_ref_t ws; typename graph_t::mut_val_ref_t wd;
              // Assumption: gx.mem(s, d) -> gx.edge_val(s, d) <= ranges[var(s)].ub() - ranges[var(d)].lb()
              // That is, if the relation exists, it's at least as strong as the bounds.
              if(gy.lookup(s, 0, &ws) && gy.lookup(0, d, &wd))
                g_rx_iy.add_edge(s, ws.get() + wd.get(), d);
            }
          }
          delta.clear();
          // Similarly, should use a SubGraph view.
          graph_t g_ry(GrOps::meet(gy, g_rx_iy, is_closed));
          assert(check_potential(g_rx, pot_rx));
          if(!is_closed)
          {

            SubGraph<graph_t> g_ry_excl(g_ry, 0);
            GrOps::close_after_meet(g_ry_excl, pot_ry, gy, g_rx_iy, delta);
            GrOps::apply_delta(g_ry, delta);
          }
           
          // We now have the relevant set of relations. Because g_rx and g_ry are closed,
          // the result is also closed.
          Wt_min min_op;
          graph_t join_g(GrOps::join(g_rx, g_ry));

          // Now reapply the missing independent relations.
          // Need to derive vert_ids from lb_up/lb_down, and make sure the vertices exist
          std::vector<vert_id> lb_up;
          std::vector<vert_id> lb_down;
          std::vector<vert_id> ub_up;
          std::vector<vert_id> ub_down;

          typename graph_t::mut_val_ref_t wx;
          typename graph_t::mut_val_ref_t wy;
          for(vert_id v : gx_excl.verts())
          {
            if(gx.lookup(0, v, &wx) && gy.lookup(0, v, &wy))
            {
              if(wx.get() < wy.get())
                ub_up.push_back(v);
              if(wy.get() < wx.get())
                ub_down.push_back(v);
            }
            if(gx.lookup(v, 0, &wx) && gy.lookup(v, 0, &wy))
            {
              if(wx.get() < wy.get())
                lb_down.push_back(v);
              if(wy.get() < wx.get())
                lb_up.push_back(v);
            }
          }

          for(vert_id s : lb_up)
          {
            Wt dx_s = gx.edge_val(s, 0);
            Wt dy_s = gy.edge_val(s, 0);
            for(vert_id d : ub_up)
            {
              if(s == d)
                continue;

              join_g.update_edge(s, std::max(dx_s + gx.edge_val(0, d),
					     dy_s + gy.edge_val(0, d)),
				 d, min_op);
            }
          }

          for(vert_id s : lb_down)
          {
            Wt dx_s = gx.edge_val(s, 0);
            Wt dy_s = gy.edge_val(s, 0);
            for(vert_id d : ub_down)
            {
              if(s == d)
                continue;

              join_g.update_edge(s, std::max(dx_s + gx.edge_val(0, d),
					     dy_s + gy.edge_val(0, d)),
				 d, min_op);
            }
          }

          // Conjecture: join_g remains closed.
          
          // Now garbage collect any unused vertices
          for(vert_id v : join_g.verts())
          {
            if(v == 0)
              continue;
            if(join_g.succs(v).size() == 0 && join_g.preds(v).size() == 0)
            {
              join_g.forget(v);
              if(out_revmap[v])
              {
                out_vmap.erase(*(out_revmap[v]));
                out_revmap[v] = boost::none;
              }
            }
          }
          
          // DBM_t res(join_range, out_vmap, out_revmap, join_g, join_pot);
          DBM_t res(std::move(out_vmap), std::move(out_revmap), std::move(join_g), 
                    std::move(pot_rx), vert_set_t());
          //join_g.check_adjs();
          CRAB_LOG ("zones-split",
                    crab::outs() << "Result join:\n"<<res <<"\n");
           
          return res;
        }
      }

      DBM_t operator||(DBM_t& o) {	
        crab::CrabStats::count (getDomainName() + ".count.widening");
        crab::ScopedCrabStats __st__(getDomainName() + ".widening");

        if (is_bottom())
          return o;
        else if (o.is_bottom())
          return *this;
        else {
          CRAB_LOG ("zones-split",
                    crab::outs() << "Before widening:\n"<<"DBM 1\n"<<*this<<"\n"<<"DBM 2\n"
		    <<o <<"\n");
          o.normalize();
          
          // Figure out the common renaming
          std::vector<vert_id> perm_x;
          std::vector<vert_id> perm_y;
          vert_map_t out_vmap;
          rev_map_t out_revmap;
          std::vector<Wt> widen_pot;
          vert_set_t widen_unstable(unstable);

          assert(potential.size() > 0);
          widen_pot.push_back(Wt(0));
          perm_x.push_back(0);
          perm_y.push_back(0);
          out_revmap.push_back(boost::none);
          for(auto p : vert_map)
          {
            auto it = o.vert_map.find(p.first); 
            // Variable exists in both
            if(it != o.vert_map.end())
            {
              out_vmap.insert(vmap_elt_t(p.first, perm_x.size()));
              out_revmap.push_back(p.first);

              widen_pot.push_back(potential[p.second] - potential[0]);
              perm_x.push_back(p.second);
              perm_y.push_back((*it).second);
            }
          }

          // Build the permuted view of x and y.
          assert(g.size() > 0);
          GrPerm gx(perm_x, g);            
          assert(o.g.size() > 0);
          GrPerm gy(perm_y, o.g);
         
          // Now perform the widening 
          std::vector<vert_id> destabilized;
          graph_t widen_g(GrOps::widen(gx, gy, destabilized));
          for(vert_id v : destabilized)
            widen_unstable.insert(v);

          DBM_t res(std::move(out_vmap), std::move(out_revmap), std::move(widen_g), 
                    std::move(widen_pot), std::move(widen_unstable));
           
          CRAB_LOG ("zones-split",
                    crab::outs() << "Result widening:\n"<<res <<"\n");
          return res;
        }
      }

      template<typename Thresholds>
      DBM_t widening_thresholds (DBM_t& o, const Thresholds &ts) {
        // TODO: use thresholds
        return (*this || o);
      }

      DBM_t operator&(DBM_t& o) {
        crab::CrabStats::count (getDomainName() + ".count.meet");
        crab::ScopedCrabStats __st__(getDomainName() + ".meet");

        if (is_bottom() || o.is_bottom())
          return bottom();
        else if (is_top())
          return o;
        else if (o.is_top())
          return *this;
        else{
          CRAB_LOG ("zones-split",
                    crab::outs() << "Before meet:\n"<<"DBM 1\n"<<*this<<"\n"<<"DBM 2\n"<<o
		                 <<"\n");
          normalize();
          o.normalize();
          
          // We map vertices in the left operand onto a contiguous range.
          // This will often be the identity map, but there might be gaps.
          vert_map_t meet_verts;
          rev_map_t meet_rev;

          std::vector<vert_id> perm_x;
          std::vector<vert_id> perm_y;
          std::vector<Wt> meet_pi;
          perm_x.push_back(0);
          perm_y.push_back(0);
          meet_pi.push_back(Wt(0));
          meet_rev.push_back(boost::none);
          for(auto p : vert_map)
          {
            vert_id vv = perm_x.size();
            meet_verts.insert(vmap_elt_t(p.first, vv));
            meet_rev.push_back(p.first);

            perm_x.push_back(p.second);
            perm_y.push_back(-1);
            meet_pi.push_back(potential[p.second] - potential[0]);
          }

          // Add missing mappings from the right operand.
          for(auto p : o.vert_map)
          {
            auto it = meet_verts.find(p.first);

            if(it == meet_verts.end())
            {
              vert_id vv = perm_y.size();
              meet_rev.push_back(p.first);

              perm_y.push_back(p.second);
              perm_x.push_back(-1);
              meet_pi.push_back(o.potential[p.second] - o.potential[0]);
              meet_verts.insert(vmap_elt_t(p.first, vv));
            } else {
              perm_y[(*it).second] = p.second;
            }
          }

          // Build the permuted view of x and y.
          assert(g.size() > 0);
          GrPerm gx(perm_x, g);
          assert(o.g.size() > 0);
          GrPerm gy(perm_y, o.g);

          // Compute the syntactic meet of the permuted graphs.
          bool is_closed;
          graph_t meet_g(GrOps::meet(gx, gy, is_closed));
           
          // Compute updated potentials on the zero-enriched graph
          //vector<Wt> meet_pi(meet_g.size());
          // We've warm-started pi with the operand potentials
          if(!GrOps::select_potentials(meet_g, meet_pi))
          {
            // Potentials cannot be selected -- state is infeasible.
            return bottom();
          }

          if(!is_closed)
          {
            edge_vector delta;
            SubGraph<graph_t> meet_g_excl(meet_g, 0);
	    // GrOps::close_after_meet(meet_g_excl, meet_pi, gx, gy, delta);

            if(Params::chrome_dijkstra)
              GrOps::close_after_meet(meet_g_excl, meet_pi, gx, gy, delta);
            else
              GrOps::close_johnson(meet_g_excl, meet_pi, delta);

            GrOps::apply_delta(meet_g, delta);

          // Recover updated LBs and UBs.
          #ifdef CLOSE_BOUNDS_INLINE
            Wt_min min_op;
            for(auto e : delta)
            {
              if(meet_g.elem(0, e.first.first))
                meet_g.update_edge(0, meet_g.edge_val(0, e.first.first) + e.second,
				   e.first.second, min_op);
              if(meet_g.elem(e.first.second, 0))
                meet_g.update_edge(e.first.first, meet_g.edge_val(e.first.second, 0) + e.second,
				   0, min_op);
            }
          #else
            delta.clear();
            GrOps::close_after_assign(meet_g, meet_pi, 0, delta);
            GrOps::apply_delta(meet_g, delta);
          #endif
          }
          assert(check_potential(meet_g, meet_pi)); 
          DBM_t res(std::move(meet_verts), std::move(meet_rev), std::move(meet_g), 
                    std::move(meet_pi), vert_set_t());
          CRAB_LOG ("zones-split",
                    crab::outs() << "Result meet:\n"<<res <<"\n");
          return res;
        }
      }
    
      DBM_t operator&&(DBM_t& o) {
        crab::CrabStats::count (getDomainName() + ".count.narrowing");
        crab::ScopedCrabStats __st__(getDomainName() + ".narrowing");

        if (is_bottom() || o.is_bottom())
          return bottom();
        else if (is_top ())
          return o;
        else{
          CRAB_LOG ("zones-split",
                    crab::outs() << "Before narrowing:\n"<<"DBM 1\n"<<*this<<"\n"<<"DBM 2\n"
		                 << o <<"\n");

          // FIXME: Implement properly
          // Narrowing as a no-op should be sound.
          normalize();
          DBM_t res(*this);

          CRAB_LOG ("zones-split",
                    crab::outs() << "Result narrowing:\n"<<res <<"\n");
          return res;
        }
      }	

      void normalize() {
        // dbm_canonical(_dbm);
        // Always maintained in normal form, except for widening
        #ifdef SDBM_NO_NORMALIZE
        return;
        #endif
        if(unstable.size() == 0)
          return;

        edge_vector delta;
	// GrOps::close_after_widen(g, potential, vert_set_wrap_t(unstable), delta);
        // GKG: Check
        SubGraph<graph_t> g_excl(g, 0);
        if(Params::widen_restabilize)
          GrOps::close_after_widen(g_excl, potential, vert_set_wrap_t(unstable), delta);
        else
          GrOps::close_johnson(g_excl, potential, delta);
        // Retrive variable bounds
        GrOps::close_after_assign(g, potential, 0, delta);

        GrOps::apply_delta(g, delta);

        unstable.clear();
      }

      void operator-=(variable_t v) {
        if (is_bottom ())
          return;
        normalize();

        auto it = vert_map.find (v);
        if (it != vert_map.end ()) {
          CRAB_LOG("zones-split", crab::outs() << "Before forget "<< it->second<< ": "
		                               << g <<"\n");
          g.forget(it->second);
          CRAB_LOG("zones-split", crab::outs() << "After: "<< g <<"\n");
          rev_map[it->second] = boost::none;
          vert_map.erase(v);
        }
      }

      template<typename Iterator>
      void forget (Iterator vIt, Iterator vEt) {
        if (is_bottom ())
          return;
        for (auto v: boost::make_iterator_range (vIt,vEt)) {
          auto it = vert_map.find (v);
          if (it != vert_map.end ()) {
            operator-=(v);
          }
        }
      }

      // Assumption: state is currently feasible.
      void assign(variable_t x, linear_expression_t e) {
        crab::CrabStats::count (getDomainName() + ".count.assign");
        crab::ScopedCrabStats __st__(getDomainName() + ".assign");

        if(is_bottom())
          return;
        CRAB_LOG("zones-split", crab::outs() << "Before assign: "<< *this <<"\n");
        CRAB_LOG("zones-split", crab::outs() << x<< ":="<< e <<"\n");
        normalize();

        assert(check_potential(g, potential));

        // If it's a constant, just assign the interval.
        if (e.is_constant()){
          set(x, e.constant());
        } else {
          interval_t x_int = eval_interval(e);
          std::vector<std::pair<variable_t, Wt> > diffs_lb;
          std::vector<std::pair<variable_t, Wt> > diffs_ub;
          // Construct difference constraints from the assignment
          diffcsts_of_assign(x, e, diffs_lb, diffs_ub);
          if(diffs_lb.size() > 0 || diffs_ub.size() > 0)
          {
            if(Params::special_assign)
            {
              // Allocate a new vertex for x
              vert_id v = g.new_vertex();
              assert(v <= rev_map.size());
              if(v == rev_map.size())
              {
                rev_map.push_back(x);
                potential.push_back(potential[0] + eval_expression(e));
              } else {
                potential[v] = potential[0] + eval_expression(e);
                rev_map[v] = x;
              }
              
              edge_vector delta;
              for(auto diff : diffs_lb)
              {
                delta.push_back(std::make_pair(std::make_pair(v, get_vert(diff.first)),
					       -diff.second));
              }

              for(auto diff : diffs_ub)
              {
                delta.push_back(std::make_pair(std::make_pair(get_vert(diff.first), v),
					       diff.second));
              }
                 
              // apply_delta should be safe here, as x has no edges in G.
              GrOps::apply_delta(g, delta);
              delta.clear();
              SubGraph<graph_t> g_excl(g, 0);
              GrOps::close_after_assign(g_excl, potential, v, delta);
              GrOps::apply_delta(g, delta);

              Wt_min min_op;
              if(x_int.lb().is_finite())
                g.update_edge(v, ntov::ntov(-(*(x_int.lb().number()))), 0, min_op);
              if(x_int.ub().is_finite())
                g.update_edge(0, ntov::ntov(*(x_int.ub().number())), v, min_op);
              // Clear the old x vertex
              operator-=(x);
              vert_map.insert(vmap_elt_t(x, v));
            } else {
              // Assignment as a sequence of edge additions.
              vert_id v = g.new_vertex();
              assert(v <= rev_map.size());
              if(v == rev_map.size())
              {
                rev_map.push_back(x);
                potential.push_back(Wt(0));
              } else {
                potential[v] = Wt(0);
                rev_map[v] = x;
              }
              Wt_min min_op;
              edge_vector cst_edges;

              for(auto diff : diffs_lb)
              {
                cst_edges.push_back(std::make_pair(std::make_pair(v, get_vert(diff.first)),
						   -diff.second));
              }

              for(auto diff : diffs_ub)
              {
                cst_edges.push_back(std::make_pair(std::make_pair(get_vert(diff.first), v),
						   diff.second));
              }
               
              for(auto diff : cst_edges)
              {
                vert_id src = diff.first.first;
                vert_id dest = diff.first.second;
                g.update_edge(src, diff.second, dest, min_op);
                if(!repair_potential(src, dest))
                {
                  assert(0 && "Unreachable");
                  set_to_bottom();
                }
                assert(check_potential(g, potential));
                
                close_over_edge(src, dest);
                assert(check_potential(g, potential));
              }

              if(x_int.lb().is_finite())
                g.update_edge(v, ntov::ntov(-(*(x_int.lb().number()))), 0, min_op);
              if(x_int.ub().is_finite())
                g.update_edge(0, ntov::ntov(*(x_int.ub().number())), v, min_op);

              // Clear the old x vertex
              operator-=(x);
              vert_map.insert(vmap_elt_t(x, v));
            }
          } else {
            set(x, x_int);
          }
          // CRAB_WARN("DBM only supports a cst or var on the rhs of assignment");
          // this->operator-=(x);
        }

	// g.check_adjs(); 

        assert(check_potential(g, potential));
        CRAB_LOG("zones-split", crab::outs() << "---"<< x<< ":="<< e<<"\n"<<*this <<"\n");
      }

      void apply(operation_t op, variable_t x, variable_t y, variable_t z){	
        crab::CrabStats::count (getDomainName() + ".count.apply");
        crab::ScopedCrabStats __st__(getDomainName() + ".apply");

        if(is_bottom())
          return;

        normalize();

        switch(op)
        {
          case OP_ADDITION:
          {
            assign(x, y + z);
            break;
          }
          case OP_SUBTRACTION:
          {
            assign(x, y - z);
            break;
          }
          // For mul and div, we fall back on intervals.
          case OP_MULTIPLICATION:
          {
            set(x, get_interval(y)*get_interval(z));
            break;
          }
          case OP_DIVISION:
          {
            interval_t xi(get_interval(y)/get_interval(z));
            if(xi.is_bottom())
              set_to_bottom();
            else
              set(x, xi);
            break;
          }
        }
        /*
        if (x == y){
          // --- ensure lhs does not appear on the rhs
          assign_tmp(y); 
          apply(op, x, get_tmp(), get_dbm_index(z));
          forget(get_tmp());
        }
        else if (x == z){
          // --- ensure lhs does not appear on the rhs
          assign_tmp(z); 
          apply(op, x, get_dbm_index (y), get_tmp());
          forget(get_tmp());
        }
        else{
          if (x == y && y == z)
            CRAB_ERROR("DBM: does not support x := x + x ");
          else
            apply(op, x, get_dbm_index(y), get_dbm_index(z));
        }
      */
        CRAB_LOG("zones-split",
                 crab::outs() << "---"<< x<< ":="<< y<< op<< z<<"\n"<< *this <<"\n");
      }

    
      void apply(operation_t op, variable_t x, variable_t y, Number k) {	
        crab::CrabStats::count (getDomainName() + ".count.apply");
        crab::ScopedCrabStats __st__(getDomainName() + ".apply");

        if(is_bottom())
          return;

        normalize();

        switch(op)
        {
          case OP_ADDITION:
          {
            assign(x, y + k);
            break;
          }
          case OP_SUBTRACTION:
          {
            assign(x, y - k);
            break;
          }
          // For mul and div, we fall back on intervals.
          case OP_MULTIPLICATION:
          {
            set(x, get_interval(y)*k);

            break;
          }
          case OP_DIVISION:
          {
            if(k == Wt(0))
              set_to_bottom();
            else
              set(x, get_interval(y)/k);

            break;
          }
        }

        CRAB_LOG("zones-split",
                 crab::outs() << "---"<< x<< ":="<< y<< op<< k<<"\n"<< *this <<"\n");
      }
      
      void backward_assign (variable_t x, linear_expression_t e,
			    DBM_t inv) { 
	crab::domains::BackwardAssignOps<DBM_t>::
	  assign (*this, x, e, inv);
      }
      
      void backward_apply (operation_t op,
			   variable_t x, variable_t y, Number z,
			   DBM_t inv) {
	crab::domains::BackwardAssignOps<DBM_t>::
	  apply(*this, op, x, y, z, inv);
      }
      
      void backward_apply(operation_t op,
			  variable_t x, variable_t y, variable_t z,
			  DBM_t inv) {
	crab::domains::BackwardAssignOps<DBM_t>::
	  apply(*this, op, x, y, z, inv);
      }
      
      void operator+=(linear_constraint_t cst) {
        crab::CrabStats::count (getDomainName() + ".count.add_constraints");
        crab::ScopedCrabStats __st__(getDomainName() + ".add_constraints");

	// XXX: we do nothing with unsigned linear inequalities
	if (cst.is_inequality() && cst.is_unsigned()) {
	  CRAB_WARN("unsigned inequality skipped");	  
	  return;
	}
	
        if(is_bottom())
          return;
        normalize();

        if (cst.is_tautology())
          return;

	// g.check_adjs();
      
        if (cst.is_contradiction()){
          set_to_bottom();
          return ;
        }

        if (cst.is_inequality())
        {
          if(!add_linear_leq(cst.expression()))
            set_to_bottom();
	  //  g.check_adjs();
          CRAB_LOG("zones-split",
                   crab::outs() << "--- "<< cst<< "\n"<< *this <<"\n");
          return;
        }

        if (cst.is_equality())
        {
          linear_expression_t exp = cst.expression();
          if(!add_linear_leq(exp) || !add_linear_leq(-exp))
          {
            CRAB_LOG("zones-split", crab::outs() << " ~~> _|_" <<"\n");
            set_to_bottom();
          }
	  // g.check_adjs();
          CRAB_LOG("zones-split",
                   crab::outs() << "--- "<< cst<< "\n"<< *this <<"\n");
          return;
        }

        if (cst.is_disequation())
        {
          add_disequation(cst.expression());
          return;
        }

        CRAB_WARN("Unhandled constraint in SplitDBM");

        CRAB_LOG("zones-split",
                 crab::outs() << "---"<< cst<< "\n"<< *this <<"\n");
        return;
      }
    
      void operator+=(linear_constraint_system_t csts) {  
        if(is_bottom()) return;

        for(auto cst: csts) {
          operator+=(cst);
        }
      }

      interval_t operator[](variable_t x) { 
        crab::CrabStats::count (getDomainName() + ".count.to_intervals");
        crab::ScopedCrabStats __st__(getDomainName() + ".to_intervals");

        // if (is_top())    return interval_t::top();

        if (is_bottom()) {
            return interval_t::bottom();
        } else {
          return get_interval(vert_map, g, x);
        }
      }

      void set(variable_t x, interval_t intv) {
        crab::CrabStats::count (getDomainName() + ".count.assign");
        crab::ScopedCrabStats __st__(getDomainName() + ".assign");

        if(is_bottom())
          return;

        this->operator-=(x);

        if(intv.is_top())
          return;

        vert_id v = get_vert(x);
        if(intv.ub().is_finite())
        {
          Wt ub = ntov::ntov(*(intv.ub().number()));
          potential[v] = potential[0] + ub;
          g.set_edge(0, ub, v);
        }
        if(intv.lb().is_finite())
        {
          Wt lb = ntov::ntov(*(intv.lb().number()));
          potential[v] = potential[0] + lb;
          g.set_edge(v, -lb, 0);
        }
      }

      // int_cast_operators_api

      void apply(int_conv_operation_t /*op*/, variable_t dst, variable_t src) {
        // since reasoning about infinite precision we simply assign and
        // ignore the widths.
        assign(dst, src);
      }

      // bitwise_operators_api      
      void apply(bitwise_operation_t op, variable_t x, variable_t y, variable_t z) {
        crab::CrabStats::count (getDomainName() + ".count.apply");
        crab::ScopedCrabStats __st__(getDomainName() + ".apply");

        // Convert to intervals and perform the operation
        normalize();
        this->operator-=(x); 

        interval_t yi = operator[](y);
        interval_t zi = operator[](z);
        interval_t xi = interval_t::bottom();
        switch (op) {
          case OP_AND: {
            xi = yi.And(zi);
            break;
          }
          case OP_OR: {
            xi = yi.Or(zi);
            break;
          }
          case OP_XOR: {
            xi = yi.Xor(zi);
            break;
          }
          case OP_SHL: {
            xi = yi.Shl(zi);
            break;
          }
          case OP_LSHR: {
            xi = yi.LShr(zi);
            break;
          }
          case OP_ASHR: {
            xi = yi.AShr(zi);
            break;
          }
          default: 
            CRAB_ERROR("DBM: unreachable");
        }
        set(x, xi);
      }
    
      void apply(bitwise_operation_t op, variable_t x, variable_t y, Number k) {
        crab::CrabStats::count (getDomainName() + ".count.apply");
        crab::ScopedCrabStats __st__(getDomainName() + ".apply");

        // Convert to intervals and perform the operation
        normalize();
        interval_t yi = operator[](y);
        interval_t zi(k);
        interval_t xi = interval_t::bottom();

        switch (op) {
          case OP_AND: {
            xi = yi.And(zi);
            break;
          }
          case OP_OR: {
            xi = yi.Or(zi);
            break;
          }
          case OP_XOR: {
            xi = yi.Xor(zi);
            break;
          }
          case OP_SHL: {
            xi = yi.Shl(zi);
            break;
          }
          case OP_LSHR: {
            xi = yi.LShr(zi);
            break;
          }
          case OP_ASHR: {
            xi = yi.AShr(zi);
            break;
          }
          default: 
            CRAB_ERROR("DBM: unreachable");
        }
        set(x, xi);
      }
    
      // division_operators_api
    
      void apply(div_operation_t op, variable_t x, variable_t y, variable_t z) {
        crab::CrabStats::count (getDomainName() + ".count.apply");
        crab::ScopedCrabStats __st__(getDomainName() + ".apply");

        if (op == OP_SDIV){
          apply(OP_DIVISION, x, y, z);
        }
        else{
          normalize();
          // Convert to intervals and perform the operation
          interval_t yi = operator[](y);
          interval_t zi = operator[](z);
          interval_t xi = interval_t::bottom();
      
          switch (op) {
            case OP_UDIV: {
              xi = yi.UDiv(zi);
              break;
            }
            case OP_SREM: {
              xi = yi.SRem(zi);
              break;
            }
            case OP_UREM: {
              xi = yi.URem(zi);
              break;
            }
            default: 
              CRAB_ERROR("spDBM: unreachable");
          }
          set(x, xi);
        }
      }

      void apply(div_operation_t op, variable_t x, variable_t y, Number k) {
        crab::CrabStats::count (getDomainName() + ".count.apply");
        crab::ScopedCrabStats __st__(getDomainName() + ".apply");

        if (op == OP_SDIV){
          apply(OP_DIVISION, x, y, k);
        }
        else{
          // Convert to intervals and perform the operation
          interval_t yi = operator[](y);
          interval_t zi(k);
          interval_t xi = interval_t::bottom();
      
          switch (op) {
            case OP_UDIV: {
              xi = yi.UDiv(zi);
              break;
            }
            case OP_SREM: {
              xi = yi.SRem(zi);
              break;
            }
            case OP_UREM: {
              xi = yi.URem(zi);
              break;
            }
            default: 
              CRAB_ERROR("DBM: unreachable");
          }
          set(x, xi);
        }
      }

      //! copy of x into a new fresh variable y
      void expand (variable_t x, variable_t y) {
        crab::CrabStats::count (getDomainName() + ".count.expand");
        crab::ScopedCrabStats __st__(getDomainName() + ".expand");

        if(is_bottom()) 
          return;
        
        CRAB_LOG ("zones-split",
                  crab::outs() << "Before expand " << x << " into " << y << ":\n"
		               << *this <<"\n");

        auto it = vert_map.find(y);
        if(it != vert_map.end()) {
          CRAB_ERROR("split_dbm expand operation failed because y already exists");
        }
        
        vert_id ii = get_vert(x);
        vert_id jj = get_vert(y);

        for (auto edge : g.e_preds(ii))  
          g.add_edge (edge.vert, edge.val, jj);
        
        for (auto edge : g.e_succs(ii))  
          g.add_edge (jj, edge.val, edge.vert);

	potential[jj] = potential[ii];
	
        CRAB_LOG ("zones-split",
                  crab::outs() << "After expand " << x << " into " << y << ":\n"
		               << *this <<"\n");
      }

      // dual of forget: remove all variables except [vIt,...vEt)
      template<typename Iterator>
      void project (Iterator vIt, Iterator vEt) {
        crab::CrabStats::count (getDomainName() + ".count.project");
        crab::ScopedCrabStats __st__(getDomainName() + ".project");

        if (is_bottom ())
          return;
        if (vIt == vEt) 
          return;

        normalize();

        std::vector<bool> save(rev_map.size(), false);
        for(auto x : boost::make_iterator_range(vIt, vEt))
        {
          auto it = vert_map.find(x);
          if(it != vert_map.end())
            save[(*it).second] = true;
        }

        for(vert_id v = 0; v < rev_map.size(); v++)
	{
          if(!save[v] && rev_map[v])
            operator-=((*rev_map[v]));
        }
      }

      void rename(const variable_vector_t &from, const variable_vector_t &to) {
	if (is_top () || is_bottom()) return;
	
	// renaming vert_map by creating a new vert_map since we are
	// modifying the keys.
	// rev_map is modified in-place since we only modify values.
	CRAB_LOG("zones-split",
		 crab::outs() << "Replacing {";
		 for (auto v: from) crab::outs() << v << ";";
		 crab::outs() << "} with ";
		 for (auto v: to) crab::outs() << v << ";";
		 crab::outs() << "}:\n";
		 crab::outs() << *this << "\n";);
	
	vert_map_t new_vert_map;
	for (auto kv: vert_map) {
	  ptrdiff_t pos = std::distance(from.begin(),
					std::find(from.begin(), from.end(), kv.first));
	  if (pos < from.size()) {
	    variable_t new_v(to[pos]);
	    new_vert_map.insert(vmap_elt_t(new_v, kv.second));
	    rev_map[kv.second] = new_v;
	  } else {
	    new_vert_map.insert(kv);
	  }
	}
	std::swap(vert_map, new_vert_map);

	CRAB_LOG("zones-split",
		 crab::outs () << "RESULT=" << *this << "\n");
      }
            
      void extract(const variable_t& x, linear_constraint_system_t& csts,
		   bool only_equalities){
        crab::CrabStats::count (getDomainName() + ".count.extract");
        crab::ScopedCrabStats __st__(getDomainName() + ".extract");

        normalize ();
        if (is_bottom ()) {
	  return;
	}

        auto it = vert_map.find(x);
        if(it != vert_map.end()) {
          vert_id s = (*it).second;
          if(rev_map[s]) {
            variable_t vs = *rev_map[s];
            SubGraph<graph_t> g_excl(g, 0);
            for(vert_id d : g_excl.verts()) {
              if(rev_map[d]) {
                variable_t vd = *rev_map[d];
                // We give priority to equalities since some domains
                // might not understand inequalities
                if (g_excl.elem (s, d) && g_excl.elem (d, s) &&
                    g_excl.edge_val(s, d) == 0 &&
		    g_excl.edge_val(d, s) == 0) {
                  linear_constraint_t cst (linear_expression_t(vs) == vd);
                  csts += cst;
                } else {
		  if (!only_equalities && g_excl.elem (s, d)) {
		    linear_constraint_t cst (vd - vs <= g_excl.edge_val(s, d));
		    csts += cst;
		  }
		  if (!only_equalities && g_excl.elem (d, s)) {
		    linear_constraint_t cst (vs - vd <= g_excl.edge_val(d, s));
		    csts += cst;
		  }
		}
              }
            }
          }
        }
      }

      // -- begin array_sgraph_domain_traits

      // return true iff inequality cst is unsatisfiable.
      bool is_unsat(linear_constraint_t cst) {
        if (is_bottom() || cst.is_contradiction()) 
          return true;

        if (is_top() || cst.is_tautology()) 
          return false;

        if (!cst.is_inequality()) {
          CRAB_WARN("is_unsat only supports inequalities");
          /// XXX: but it would be trivial to handle equalities
          return false;
        }
                  
        auto diffcst = diffcst_of_leq(cst);
        if (!diffcst)
          return false;

        // x - y <= k
        auto x = (*diffcst).first;
        auto y = (*diffcst).second.first;
        auto k = (*diffcst).second.second;

        typename graph_t::mut_val_ref_t w;        
        if (g.lookup(y,x,&w)) {
          return ((w + k) < 0);
        } else {
          interval_t intv_x = interval_t::top();
          interval_t intv_y = interval_t::top();
          if (g.elem(0,x) || g.elem(x,0)) {
            intv_x = interval_t(
                g.elem(x, 0) ? -Number(g.edge_val(x, 0)) : bound_t::minus_infinity(),
                g.elem(0, x) ?  Number(g.edge_val(0, x)) : bound_t::plus_infinity());
          }
          if (g.elem(0,y) || g.elem(y,0)) {
            intv_y = interval_t(
                g.elem(y, 0) ? -Number(g.edge_val(y, 0)) : bound_t::minus_infinity(),
                g.elem(0, y) ?  Number(g.edge_val(0, y)) : bound_t::plus_infinity());
          }
          if (intv_x.is_top() || intv_y.is_top()) {
            return false;
          } else  {
            return (!((intv_y - intv_x).lb() <= k));
          }
        }
      }

      void active_variables(std::vector<variable_t>& out) const {
        out.reserve(g.size());
        for (auto v: g.verts()) {
          if (rev_map[v]) 
            out.push_back((*(rev_map[v])));
        }
      }
      // -- end array_sgraph_domain_traits
      
      // Output function
      void write(crab_os& o) {

        normalize ();
        #if 0
        o << "edges={";
        for(vert_id v : g.verts())
        {
          for(vert_id d : g.succs(v))
          {
            if(!rev_map[v] || !rev_map[d])
            {
              CRAB_WARN("Edge incident to un-mapped vertex.");
              continue;
            }
            o << "(" << (*(rev_map[v])) << "," << (*(rev_map[d])) << ":"
	             << g.edge_val(v,d) << ")";
          }
        }
        o << "}";
        crab::outs() << "rev_map={";
        for(unsigned i=0, e = rev_map.size(); i!=e; i++) {
          if (rev_map[i])
            crab::outs() << *(rev_map[i]) << "(" << i << ");";
        }
        crab::outs() << "}\n";
        #endif 

        if(is_bottom()){
          o << "_|_";
          return;
        }
        else if (is_top()){
          o << "{}";
          return;
        }
        else
        {
          // Intervals
          bool first = true;
          o << "{";
          // Extract all the edges
          SubGraph<graph_t> g_excl(g, 0);
          for(vert_id v : g_excl.verts())
          {
            if(!rev_map[v])
              continue;
            if(!g.elem(0, v) && !g.elem(v, 0))
             continue; 
            interval_t v_out = interval_t(
                g.elem(v, 0) ? -Number(g.edge_val(v, 0)) : bound_t::minus_infinity(),
                g.elem(0, v) ? Number(g.edge_val(0, v)) : bound_t::plus_infinity());
            
            if(first)
              first = false;
            else
              o << ", ";
            o << *(rev_map[v]) << " -> " << v_out;
          }

          for(vert_id s : g_excl.verts())
          {
            if(!rev_map[s])
              continue;
            variable_t vs = *rev_map[s];
            for(vert_id d : g_excl.succs(s))
            {
              if(!rev_map[d])
                continue;
              variable_t vd = *rev_map[d];
              if(first)
                first = false;
              else
                o << ", ";
              o << vd << "-" << vs << "<=" << g_excl.edge_val(s, d);
            }
          }
          o << "}";

	  // linear_constraint_system_t inv = to_linear_constraint_system ();
	  // o << inv;
        }
      }

      linear_constraint_system_t to_linear_constraint_system () {

        normalize ();

        linear_constraint_system_t csts;
    
        if(is_bottom ()) {
          csts += linear_constraint_t::get_false();
          return csts;
        }

        // Extract all the edges

        SubGraph<graph_t> g_excl(g, 0);

        for(vert_id v : g_excl.verts())
        {
          if(!rev_map[v])
            continue;
          if(g.elem(v, 0))
            csts += linear_constraint_t(linear_expression_t(*rev_map[v]) >= -g.edge_val(v, 0));
          if(g.elem(0, v))
            csts += linear_constraint_t(linear_expression_t(*rev_map[v]) <= g.edge_val(0, v));
        }

        for(vert_id s : g_excl.verts())
        {
          if(!rev_map[s])
            continue;
          variable_t vs = *rev_map[s];
          for(vert_id d : g_excl.succs(s))
          {
            if(!rev_map[d])
              continue;
            variable_t vd = *rev_map[d];
            csts += linear_constraint_t(vd - vs <= linear_expression_t(g_excl.edge_val(s, d)));
          }
        }
        return csts;
      }

      static std::string getDomainName () {
        return "SplitDBM";
      }

    }; // class SplitDBM_

    #if 1
    template<class Number, class VariableName,
	     class Params = SDBM_impl::DefaultParams<Number>>
    using SplitDBM = SplitDBM_<Number,VariableName,Params>;     
    #else
    // Quick wrapper which uses shared references with copy-on-write.
    template<class Number, class VariableName,
	     class Params=SDBM_impl::DefaultParams<Number>>
    class SplitDBM:
      public abstract_domain<Number, VariableName,
			     SplitDBM<Number,VariableName,Params> > {

      typedef SplitDBM<Number, VariableName, Params> DBM_t;
      typedef abstract_domain<Number,VariableName,DBM_t> abstract_domain_t;
      
    public:
      
      using typename abstract_domain_t::linear_expression_t;
      using typename abstract_domain_t::linear_constraint_t;
      using typename abstract_domain_t::linear_constraint_system_t;
      using typename abstract_domain_t::variable_t;
      using typename abstract_domain_t::number_t;
      using typename abstract_domain_t::varname_t;
      using typename abstract_domain_t::variable_vector_t;      
      typedef typename linear_constraint_t::kind_t constraint_kind_t;
      typedef interval<Number>  interval_t;

    public:
      
      typedef SplitDBM_<Number, VariableName, Params> dbm_impl_t;
      typedef std::shared_ptr<dbm_impl_t> dbm_ref_t;

      SplitDBM(dbm_ref_t _ref) : norm_ref(_ref) { }

      SplitDBM(dbm_ref_t _base, dbm_ref_t _norm) 
        : base_ref(_base), norm_ref(_norm) { }
       

      DBM_t create(dbm_impl_t&& t) {
        return std::make_shared<dbm_impl_t>(std::move(t));
      }

      DBM_t create_base(dbm_impl_t&& t) {
        dbm_ref_t base = std::make_shared<dbm_impl_t>(t);
        dbm_ref_t norm = std::make_shared<dbm_impl_t>(std::move(t));  
        return DBM_t(base, norm);
      }

      void lock(void) {
        // Allocate a fresh copy.
        if(!norm_ref.unique())
          norm_ref = std::make_shared<dbm_impl_t>(*norm_ref);
        base_ref.reset();
      }

    public:

      static DBM_t top() { return SplitDBM(false); }
    
      static DBM_t bottom() { return SplitDBM(true); }

      SplitDBM(bool is_bottom = false)
        : norm_ref(std::make_shared<dbm_impl_t>(is_bottom)) { }

      SplitDBM(const DBM_t& o)
        : base_ref(o.base_ref), norm_ref(o.norm_ref)
      { }

      SplitDBM& operator=(const DBM_t& o) {
	if (this != &o) {
	  base_ref = o.base_ref;
	  norm_ref = o.norm_ref;
	}
        return *this;
      }

      dbm_impl_t& base(void) {
        if(base_ref)
          return *base_ref;
        else
          return *norm_ref;
      }
      dbm_impl_t& norm(void) { return *norm_ref; }

      bool is_bottom() { return norm().is_bottom(); }
      bool is_top() { return norm().is_top(); }
      bool operator<=(DBM_t& o) { return norm() <= o.norm(); }
      void operator|=(DBM_t o) { lock(); norm() |= o.norm(); }
      DBM_t operator|(DBM_t o) { return create(norm() | o.norm()); }
      //DBM_t operator||(DBM_t o) { return create_base(base() || o.norm()); }
      DBM_t operator||(DBM_t o) { return create(norm() || o.norm()); }
      DBM_t operator&(DBM_t o) { return create(norm() & o.norm()); }
      DBM_t operator&&(DBM_t o) { return create(norm() && o.norm()); }

      template<typename Thresholds>
      DBM_t widening_thresholds (DBM_t o, const Thresholds &ts) {
        //return create_base(base().template widening_thresholds<Thresholds>(o.norm(), ts));
	return create(norm().template widening_thresholds<Thresholds>(o.norm(), ts));
      }

      void normalize() { norm().normalize(); }
      void operator+=(linear_constraint_system_t csts) { lock(); norm() += csts; } 
      void operator-=(variable_t v) { lock(); norm() -= v; }
      interval_t operator[](variable_t x) { return norm()[x]; }
      void set(variable_t x, interval_t intv) { lock(); norm().set(x, intv); }

      template<typename Iterator>
      void forget (Iterator vIt, Iterator vEt) { lock(); norm().forget(vIt, vEt); }
      void assign(variable_t x, linear_expression_t e) { lock(); norm().assign(x, e); }
      void apply(operation_t op, variable_t x, variable_t y, Number k) {
        lock(); norm().apply(op, x, y, k);
      }
      void backward_assign(variable_t x, linear_expression_t e,
			   DBM_t invariant) {
	lock(); norm().backward_assign(x, e, invariant.norm());
      }
      void backward_apply(operation_t op,
			  variable_t x, variable_t y, Number k,
			  DBM_t invariant) {
	lock(); norm().backward_apply(op, x, y, k, invariant.norm());
      }
      void backward_apply(operation_t op,
			  variable_t x, variable_t y, variable_t z,
			  DBM_t invariant) {
	lock(); norm().backward_apply(op, x, y, z, invariant.norm());
      }	
      void apply(int_conv_operation_t op, variable_t dst, variable_t src) {
        lock(); norm().apply(op, dst, src);
      }
      void apply(bitwise_operation_t op, variable_t x, variable_t y, Number k) {
        lock(); norm().apply(op, x, y, k);
      }
      void apply(bitwise_operation_t op, variable_t x, variable_t y, variable_t z) {
        lock(); norm().apply(op, x, y, z);
      }
      void apply(operation_t op, variable_t x, variable_t y, variable_t z) {
        lock(); norm().apply(op, x, y, z);
      }
      void apply(div_operation_t op, variable_t x, variable_t y, variable_t z) {
        lock(); norm().apply(op, x, y, z);
      }
      void apply(div_operation_t op, variable_t x, variable_t y, Number k) {
        lock(); norm().apply(op, x, y, k);
      }
      void expand (variable_t x, variable_t y) { lock(); norm().expand(x, y); }

      template<typename Iterator>
      void project (Iterator vIt, Iterator vEt) { lock(); norm().project(vIt, vEt); }

      void rename(const variable_vector_t &from, const variable_vector_t &to)
      { lock(); norm().rename(from, to); }
      
      void extract(const variable_t& x, linear_constraint_system_t& csts,
		   bool only_equalities)
      { lock(); norm().extract(x, csts, only_equalities); }

      void write(crab_os& o) { norm().write(o); }

      linear_constraint_system_t to_linear_constraint_system () {
        return norm().to_linear_constraint_system();
      }
      static std::string getDomainName () { return dbm_impl_t::getDomainName(); }

      bool is_unsat (linear_constraint_t cst){ return norm().is_unsat(cst);}
      void active_variables(std::vector<variable_t>& out){ norm().active_variables(out);}
      
    protected:  
      dbm_ref_t base_ref;  
      dbm_ref_t norm_ref;
    };
    #endif 

    template<typename Number, typename VariableName>
    class domain_traits <SplitDBM<Number,VariableName> > {
     public:

      typedef SplitDBM<Number,VariableName> sdbm_domain_t;
      typedef ikos::variable<Number, VariableName> variable_t;
      
      template<class CFG>
      static void do_initialization (CFG cfg) { }

      static void expand (sdbm_domain_t& inv, variable_t x, variable_t new_x) {
        inv.expand (x, new_x);
      }
    
      static void normalize (sdbm_domain_t& inv) {
        inv.normalize();
      }
    
      template <typename Iter>
      static void forget (sdbm_domain_t& inv, Iter it, Iter end){
        inv.forget (it, end);
      }

      template <typename Iter>
      static void project (sdbm_domain_t& inv, Iter it, Iter end) {
        inv.project (it, end);
      }
    };


    template<typename Number, typename VariableName, typename Params>    
    class reduced_domain_traits<SplitDBM<Number, VariableName, Params>> {
    public:
      typedef SplitDBM<Number, VariableName, Params> sdbm_domain_t;
      typedef typename sdbm_domain_t::variable_t variable_t;
      typedef typename sdbm_domain_t::linear_constraint_system_t linear_constraint_system_t;
      
      static void extract(sdbm_domain_t& dom, const variable_t& x,
			  linear_constraint_system_t& csts, bool only_equalities)
      { dom.extract(x, csts, only_equalities); }
    };
  
    template<typename Number, typename VariableName>
    struct array_sgraph_domain_traits <SplitDBM<Number,VariableName> > {
      typedef SplitDBM<Number,VariableName> sdbm_domain_t;
      typedef typename sdbm_domain_t::linear_constraint_t linear_constraint_t;
      typedef ikos::variable<Number, VariableName> variable_t;
      
      static bool is_unsat(sdbm_domain_t &inv, linear_constraint_t cst) 
      { return inv.is_unsat(cst); }
      
      static void active_variables(sdbm_domain_t &inv, std::vector<variable_t>& out) 
      { inv.active_variables(out); }
    };
  
  } // namespace domains
} // namespace crab

#pragma GCC diagnostic pop
