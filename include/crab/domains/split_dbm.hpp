/*******************************************************************************
 *
 * A re-engineered implementation of the Difference Bound Matrix domain,
 * which maintains bounds and relations separately.
 *
 * Closure operations based on the paper "Fast and Flexible Difference Constraint
 * Propagation for DPLL(T)" by Cotton and Maler.
 *
 * Graeme Gange (gkgange@unimelb.edu.au)
 ******************************************************************************/

#ifndef SPLIT_DBM_HPP
#define SPLIT_DBM_HPP

// Uncomment for enabling debug information
//#include <crab/common/dbg.hpp>

#include <crab/common/types.hpp>
#include <crab/common/sparse_graph.hpp>
#include <crab/common/pt_graph.hpp>
#include <crab/common/ht_graph.hpp>
#include <crab/common/adapt_sgraph.hpp>
#include <crab/common/graph_ops.hpp>
#include <crab/domains/linear_constraints.hpp>
#include <crab/domains/intervals.hpp>
#include <crab/domains/patricia_trees.hpp>
#include <crab/domains/numerical_domains_api.hpp>
#include <crab/domains/bitwise_operators_api.hpp>
#include <crab/domains/division_operators_api.hpp>

#include <unordered_set>

#include <boost/optional.hpp>
#include <boost/unordered_set.hpp>
#include <boost/unordered_map.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/container/flat_map.hpp>

#define CLOSE_BOUNDS_INLINE
// #define CHECK_POTENTIAL
//#define SDBM_NO_NORMALIZE

using namespace boost;
using namespace std;

namespace crab {

  namespace domains {

     using namespace ikos;

     namespace SDBM_impl {
       // translate from Number to dbm val_t type
       template<typename Number, typename Wt>
       class NtoV {
       public:
         static Wt ntov(const Number& n) { 
           return (Wt) n;
         }
       };
     }; // end namespace SDBM_impl


    template<class Number, class VariableName>
    class SplitDBM: public writeable,
               public numerical_domain<Number, VariableName >,
               public bitwise_operators<Number,VariableName >,
               public division_operators<Number, VariableName >{
     public:
      using typename numerical_domain< Number, VariableName >::linear_expression_t;
      using typename numerical_domain< Number, VariableName >::linear_constraint_t;
      using typename numerical_domain< Number, VariableName >::linear_constraint_system_t;
      using typename numerical_domain< Number, VariableName >::variable_t;
      using typename numerical_domain< Number, VariableName >::number_t;
      using typename numerical_domain< Number, VariableName >::varname_t;
      
      typedef typename linear_constraint_t::kind_t constraint_kind_t;
      typedef interval<Number>  interval_t;

     private:
      typedef bound<Number>  bound_t;
      // Can't use separate_domain directly, as we need to
      // retrofit some operations onto the join.
      typedef patricia_tree< VariableName, interval_t > ranges_t;
      typedef typename ranges_t::key_binary_op_t key_binary_op_t;
      
      // Eventually break this out into a template param
      //typedef Number Wt;
      typedef long Wt;

      typedef SDBM_impl::NtoV<Number, Wt> ntov;

      //typedef SparseWtGraph<Wt> graph_t;
      //typedef PtGraph<Wt> graph_t;
      //typedef HtGraph<Wt> graph_t;
      typedef AdaptGraph<Wt> graph_t;
      typedef typename graph_t::vert_id vert_id;
      typedef boost::container::flat_map<variable_t, vert_id> vert_map_t;
      typedef typename vert_map_t::value_type vmap_elt_t;
      typedef vector< boost::optional<variable_t> > rev_map_t;

      typedef SplitDBM<Number, VariableName> DBM_t;

      typedef GraphOps<graph_t> GrOps;
      typedef GraphPerm<graph_t> GrPerm;
      typedef typename GrOps::edge_vector edge_vector;
      // < <x, y>, k> == x - y <= k.
      typedef pair< pair<VariableName, VariableName>, Wt > diffcst_t;

      typedef std::unordered_set<vert_id> vert_set_t;

      protected:
        
      //================
      // Domain data
      //================
      // GKG: ranges are now maintained in the graph
//      ranges_t ranges; // Intervals for each variable
      vert_map_t vert_map; // Mapping from variables to vertices
      rev_map_t rev_map;
      graph_t g; // The underlying relation graph
      vector<Wt> potential; // Stored potential for the vertex

      vert_set_t unstable;

      bool _is_bottom;

   public:
      SplitDBM(bool is_bottom = false):
        writeable(), _is_bottom(is_bottom)
      {
        g.growTo(1);  // Allocate the zero vector
        potential.push_back(Wt(0));
        rev_map.push_back(none);
      }

      // FIXME: Rewrite to avoid copying if o is _|_
      SplitDBM(const DBM_t& o)
        : writeable(),
          numerical_domain<Number, VariableName >(),
          bitwise_operators< Number, VariableName >(),
          division_operators< Number, VariableName >(),
          vert_map(o.vert_map),
          rev_map(o.rev_map),
          g(o.g),
          potential(o.potential),
          unstable(o.unstable),
          _is_bottom(false)
      {
        if(o._is_bottom)
          set_to_bottom();

        if(!_is_bottom)
          assert(g.size() > 0);
      }

      SplitDBM(DBM_t&& o)
        : vert_map(std::move(o.vert_map)), rev_map(std::move(o.rev_map)),
          g(std::move(o.g)), potential(std::move(o.potential)),
          unstable(std::move(o.unstable)),
          _is_bottom(o._is_bottom)
      { }

      // We should probably use the magical rvalue ownership semantics stuff.
      SplitDBM(vert_map_t& _vert_map, rev_map_t& _rev_map, graph_t& _g, vector<Wt>& _potential,
        vert_set_t& _unstable)
        : writeable(),
          numerical_domain<Number, VariableName >(),
          bitwise_operators< Number, VariableName >(),
          division_operators< Number, VariableName >(),
          /* ranges(_ranges),*/ vert_map(_vert_map), rev_map(_rev_map), g(_g), potential(_potential),
          unstable(_unstable),
          _is_bottom(false)
      {
        CRAB_WARN("Non-moving constructor.");
        assert(g.size() > 0);
      }
      
      SplitDBM(vert_map_t&& _vert_map, rev_map_t&& _rev_map, graph_t&& _g, vector<Wt>&& _potential, vert_set_t&& _unstable)
        : writeable(),
          numerical_domain<Number, VariableName >(),
          bitwise_operators< Number, VariableName >(),
          division_operators< Number, VariableName >(),
          vert_map(std::move(_vert_map)), rev_map(std::move(_rev_map)), g(std::move(_g)), potential(std::move(_potential)),
          unstable(std::move(_unstable)),
          _is_bottom(false)
      { assert(g.size() > 0); }


      // FIXME: Add a move constructor
      SplitDBM& operator=(const SplitDBM& o)
      {
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

      SplitDBM& operator=(SplitDBM&& o)
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
       
     private:

      /*
      void forget(vector<int> idxs) {
        dbm ret = NULL;
        ret = dbm_forget_array(&idxs[0], idxs.size(), _dbm);
        dbm_dealloc(_dbm);
        swap(_dbm, ret);
      }
      */

      void set_to_bottom() {
        // ranges.clear();
        vert_map.clear();
        rev_map.clear();
        g.clear();
        potential.clear();
        unstable.clear();
        _is_bottom = true;
      }

      // check satisfiability of cst using intervals
      // Only to be used if cst is too hard for dbm
      bool intervals_check_sat(linear_constraint_t cst)  {
        if (is_top())    return true;
        if (is_bottom()) return false;

        auto vars = cst.variables();
        ranges_t inv;
        for(auto v: vars)
          inv.set (v.name(), operator[](v.name));
        inv += cst;
        return !inv.is_bottom();
      }

     public:

      static DBM_t top() { return SplitDBM(false); }
    
      static DBM_t bottom() { return SplitDBM(true); }
    
     public:

      bool is_bottom() const {
//        if(!_is_bottom && g.has_negative_cycle())
//          _is_bottom = true;
        return _is_bottom;
      }
    
      bool is_top() {
        if(_is_bottom)
          return false;
        return g.is_empty();
      }
    
      bool operator<=(DBM_t& o)  {
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
//          interval_po po;
//           if(!ranges.leq(o.ranges, po))
//             return false;

//          CRAB_DEBUG("operator<=: ", *this, "<=?", o);

          if(vert_map.size() < o.vert_map.size())
            return false;

          // Set up a mapping from o to this.
          vector<unsigned int> vert_renaming(o.g.size(),-1);
          vert_renaming[0] = 0;
          for(auto p : o.vert_map)
          {
            auto it = vert_map.find(p.first);
            // We can't have this <= o if we're missing some
            // vertex.
            if(it == vert_map.end())
              return false;
            vert_renaming[p.second] = (*it).second;
          }

          assert(g.size() > 0);
          GrPerm g_perm(vert_renaming, g);

          for(vert_id ox : o.g.verts())
          {
            assert(vert_renaming[ox] != -1);
            vert_id x = vert_renaming[ox];
            for(vert_id oy : o.g.succs(ox))
            {
              assert(vert_renaming[ox] != -1);
              vert_id y = vert_renaming[oy];
              Wt ow = o.g.edge_val(ox, oy);

              if(g_perm.elem(x, y) && (g_perm.edge_val(x, y) <= ow))
                continue;

              if(!g_perm.elem(x, 0) || !g_perm.elem(0, y))
                return false;
              if(!(g_perm.edge_val(x, 0) + g_perm.edge_val(0, y) <= ow))
                return false;
            }
          }
          return true;
        }
      }

      class Wt_max {
      public:
       Wt_max() { } 
       Wt apply(const Wt& x, const Wt& y) { return max(x, y); }
       bool default_is_absorbing() { return true; }
      };

      class Wt_min {
      public:
        Wt_min() { }
        Wt apply(const Wt& x, const Wt& y) { return min(x, y); }
        bool default_is_absorbing() { return false; }
      };

      vert_id get_vert(VariableName v)
      {
        auto it = vert_map.find(variable_t(v));
        if(it != vert_map.end())
          return (*it).second;

        vert_id vert(g.new_vertex());
        vert_map.insert(vmap_elt_t(variable_t(v), vert)); 
        // Initialize 
        assert(vert <= rev_map.size());
        if(vert < rev_map.size())
        {
          potential[vert] = Wt(0);
          rev_map[vert] = v;
        } else {
          potential.push_back(Wt(0));
          rev_map.push_back(variable_t(v));
        }
        vert_map.insert(vmap_elt_t(v, vert));

        assert(vert != 0);

        return vert;
      }

      vert_id get_vert(graph_t& g, vert_map_t& vmap, rev_map_t& rmap,
          vector<Wt>& pot, VariableName v)
      {
        auto it = vmap.find(variable_t(v));
        if(it != vmap.end())
          return (*it).second;

        vert_id vert(g.new_vertex());
        vmap.insert(vmap_elt_t(variable_t(v), vert)); 
        // Initialize 
        assert(vert <= rmap.size());
        if(vert < rmap.size())
        {
          pot[vert] = Wt(0);
          rmap[vert] = v;
        } else {
          pot.push_back(Wt(0));
          rmap.push_back(variable_t(v));
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

      // FIXME: can be done more efficient
      void operator|=(DBM_t o) {
        *this = *this | o;
      }

      DBM_t operator|(DBM_t o) {
        if (is_bottom() || o.is_top ())
          return o;
        else if (is_top () || o.is_bottom())
          return *this;
        else {
          CRAB_DEBUG ("Before join:\n","DBM 1\n",*this,"\n","DBM 2\n",o);

          normalize();
          o.normalize();

          assert(check_potential(g, potential));
          assert(check_potential(o.g, o.potential));

          // Figure out the common renaming, initializing the
          // resulting potentials as we go.
          vector<vert_id> perm_x;
          vector<vert_id> perm_y;
          vector<variable_t> perm_inv;

          vector<Wt> pot_rx;
          vector<Wt> pot_ry;
          vert_map_t out_vmap;
          rev_map_t out_revmap;
          // Add the zero vertex
          assert(potential.size() > 0);
          pot_rx.push_back(0);
          pot_ry.push_back(0);
          perm_x.push_back(0);
          perm_y.push_back(0);
          out_revmap.push_back(none);

          for(auto p : vert_map)
          {
            auto it = o.vert_map.find(p.first); 
            // Variable exists in both
            if(it != o.vert_map.end())
            {
              out_vmap.insert(vmap_elt_t(p.first, perm_x.size()));
              out_revmap.push_back(p.first);

              pot_rx.push_back(potential[p.second] - potential[0]);
              pot_ry.push_back(o.potential[p.second] - o.potential[0]);
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
              if(gx.elem(s, 0) && gx.elem(0, d))
                g_ix_ry.add_edge(s, gx.edge_val(s, 0) + gx.edge_val(0, d), d);
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
              // Assumption: gx.mem(s, d) -> gx.edge_val(s, d) <= ranges[var(s)].ub() - ranges[var(d)].lb()
              // That is, if the relation exists, it's at least as strong as the bounds.
              if(gy.elem(s, 0) && gy.elem(0, d))
                g_rx_iy.add_edge(s, gy.edge_val(s, 0) + gy.edge_val(0, d), d);
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
          vector<vert_id> lb_up;
          vector<vert_id> lb_down;
          vector<vert_id> ub_up;
          vector<vert_id> ub_down;

          for(vert_id v : gx_excl.verts())
          {
            if(gx.elem(0, v) && gy.elem(0, v))
            {
              if(gx.edge_val(0, v) < gy.edge_val(0, v))
                ub_up.push_back(v);
              if(gy.edge_val(0, v) < gx.edge_val(0, v))
                ub_down.push_back(v);
            }
            if(gx.elem(v, 0) && gy.elem(v, 0))
            {
              if(gx.edge_val(v, 0) < gy.edge_val(v, 0))
                lb_down.push_back(v);
              if(gy.edge_val(v, 0) < gx.edge_val(v, 0))
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

              join_g.update_edge(s, max(dx_s + gx.edge_val(0, d), dy_s + gy.edge_val(0, d)), d, min_op);
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

              join_g.update_edge(s, max(dx_s + gx.edge_val(0, d), dy_s + gy.edge_val(0, d)), d, min_op);
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
          
//          DBM_t res(join_range, out_vmap, out_revmap, join_g, join_pot);
          DBM_t res(std::move(out_vmap), std::move(out_revmap), std::move(join_g), std::move(pot_rx), vert_set_t());
//          join_g.check_adjs();
          CRAB_DEBUG ("Result join:\n",res);
           
          return res;
        }
      }

      DBM_t operator||(DBM_t o) {	
        if (is_bottom())
          return o;
        else if (o.is_bottom())
          return *this;
        else {
          CRAB_DEBUG ("Before widening:\n","DBM 1\n",*this,"\n","DBM 2\n",o);
          o.normalize();
          
          // Figure out the common renaming
          vector<vert_id> perm_x;
          vector<vert_id> perm_y;
          vert_map_t out_vmap;
          rev_map_t out_revmap;
          vector<Wt> widen_pot;
          vert_set_t widen_unstable(unstable);

          assert(potential.size() > 0);
          widen_pot.push_back(Wt(0));
          perm_x.push_back(0);
          perm_y.push_back(0);
          out_revmap.push_back(none);
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
          vector<vert_id> destabilized;
          graph_t widen_g(GrOps::widen(gx, gy, destabilized));
          for(vert_id v : destabilized)
            widen_unstable.insert(v);

          DBM_t res(std::move(out_vmap), std::move(out_revmap), std::move(widen_g), std::move(widen_pot), std::move(widen_unstable));

          // GKG: need to mark changes so we can restore closure
           
          CRAB_DEBUG ("Result widening:\n",res);
          return res;
        }
      }

      DBM_t operator&(DBM_t o) {
        if (is_bottom() || o.is_bottom())
          return bottom();
        else if (is_top())
          return o;
        else if (o.is_top())
          return *this;
        else{
          CRAB_DEBUG ("Before meet:\n","DBM 1\n",*this,"\n","DBM 2\n",o);
          normalize();
          o.normalize();
          
          // We map vertices in the left operand onto a contiguous range.
          // This will often be the identity map, but there might be gaps.
          vert_map_t meet_verts;
          rev_map_t meet_rev;

          vector<vert_id> perm_x;
          vector<vert_id> perm_y;
          vector<Wt> meet_pi;
          perm_x.push_back(0);
          perm_y.push_back(0);
          meet_pi.push_back(Wt(0));
          meet_rev.push_back(none);
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
            GrOps::close_after_meet(meet_g_excl, meet_pi, gx, gy, delta);
            GrOps::apply_delta(meet_g, delta);

          // Recover updated LBs and UBs.
#ifdef CLOSE_BOUNDS_INLINE
            Wt_min min_op;
            for(auto e : delta)
            {
              if(meet_g.elem(0, e.first.first))
                meet_g.update_edge(0, meet_g.edge_val(0, e.first.first) + e.second, e.first.second, min_op);
              if(meet_g.elem(e.first.second, 0))
                meet_g.update_edge(e.first.first, meet_g.edge_val(e.first.second, 0) + e.second, e.first.first, min_op);
            }
#else
            delta.clear();
            GrOps::close_after_assign(meet_g, meet_pi, 0, delta);
            GrOps::apply_delta(meet_g, delta);
#endif
          }
          assert(check_potential(meet_g, meet_pi)); 
          DBM_t res(std::move(meet_verts), std::move(meet_rev), std::move(meet_g), std::move(meet_pi), vert_set_t());
          CRAB_DEBUG ("Result meet:\n",res);
          return res;
        }
      }
    
      DBM_t operator&&(DBM_t o) {
        if (is_bottom() || o.is_bottom())
          return bottom();
        else if (is_top ())
          return o;
        else{
          CRAB_DEBUG ("Before narrowing:\n","DBM 1\n",*this,"\n","DBM 2\n",o);

          // FIXME: Implement properly
          // Narrowing as a no-op should be sound.
          normalize();
          DBM_t res(*this);

          CRAB_DEBUG ("Result narrowing:\n",res);
          return res;
        }
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

      void normalize() {
        // dbm_canonical(_dbm);
        // Always maintained in normal form, except for widening
#ifdef SDBM_NO_NORMALIZE
        return;
#endif
        if(unstable.size() == 0)
          return;

        edge_vector delta;
        GrOps::close_after_widen(g, potential, vert_set_wrap_t(unstable), delta);
        GrOps::apply_delta(g, delta);

        unstable.clear();
      }

      void operator-=(VariableName v) {
        if (is_bottom ())
          return;
        normalize();

//        ranges.remove(v);
        auto it = vert_map.find (v);
        if (it != vert_map.end ()) {
          CRAB_DEBUG("Before forget ", it->second, ": ", g);
          g.forget(it->second);
          CRAB_DEBUG("After: ", g);
          rev_map[it->second] = boost::none;
          vert_map.erase(v);
        }
      }

      template<typename Iterator>
      void forget (Iterator vIt, Iterator vEt) {
        if (is_bottom ())
          return;
        // CRAB_WARN("forget not implemented.");
        for (auto v: boost::make_iterator_range (vIt,vEt)) {
          auto it = vert_map.find (v);
          if (it != vert_map.end ()) {
            operator-=(v);
          }
        }
      }

      // Evaluate the potential value of a variable.
      Wt pot_value(variable_t v)
      {
        auto it = vert_map.find(v); 
        if(it != vert_map.end())
          return potential[(*it).second];

//        interval_t r(get_interval(ranges,v));
//        if(r.lb().is_finite())
//          return (Wt) (*(r.lb().number()));
//        if(r.ub().is_finite())
//          return (Wt) (*(r.ub().number()));
        return ((Wt) 0);
      }

      //Wt pot_value(variable_t v, ranges_t& ranges, vector<Wt>& potential)
      Wt pot_value(variable_t v, vector<Wt>& potential)
      {
        auto it = vert_map.find(v); 
        if(it != vert_map.end())
          return potential[(*it).second];

//        interval_t r(get_interval(ranges,v));
//        if(r.lb().is_finite())
//          return (Wt) (*(r.lb().number()));
//        if(r.ub().is_finite())
//          return (Wt) (*(r.ub().number()));
        return ((Wt) 0);
      }

      // Evaluate an expression under the chosen potentials
      Wt eval_expression(linear_expression_t e)
      {
        Wt v(ntov::ntov(e.constant())); 
        for(auto p : e)
        {
          v += (pot_value(p.second) - potential[0])*(ntov::ntov(p.first));
        }
        return v;
      }
      
      interval_t eval_interval(linear_expression_t e)
      {
        interval_t r = e.constant();
        for (auto p : e)
          r += p.first * operator[](p.second.name());

        return r;
      }

      // Turn an assignment into a set of difference constraints.
      void diffcsts_of_assign(VariableName x, linear_expression_t exp,
          vector<pair<VariableName, Wt> >& lb, vector<pair<VariableName,Wt> >& ub)
      {
        {
          // Process upper bounds.
          optional<VariableName> unbounded_ubvar;
          Wt exp_ub(ntov::ntov(exp.constant()));
          vector< pair<VariableName, Wt> > ub_terms;
          for(auto p : exp)
          {
            Wt coeff(ntov::ntov(p.first));
            if(p.first < Wt(0))
            {
              // Can't do anything with negative coefficients.
              bound_t y_lb = operator[](p.second.name()).lb();
              if(y_lb.is_infinite())
                goto assign_ub_finish;
              exp_ub += ntov::ntov(*(y_lb.number()))*coeff;
            } else {
              VariableName y(p.second.name());
              bound_t y_ub = operator[](y).ub(); 
              if(y_ub.is_infinite())
              {
                if(unbounded_ubvar || coeff != Wt(1))
                  goto assign_ub_finish;
                unbounded_ubvar = y;
              } else {
                Wt ymax(ntov::ntov(*(y_ub.number())));
                exp_ub += ymax*coeff;
                ub_terms.push_back(make_pair(y, ymax));
              }
            }
          }

          if(unbounded_ubvar)
          {
            // There is exactly one unbounded variable. 
            ub.push_back(make_pair(*unbounded_ubvar, exp_ub));
          } else {
            for(auto p : ub_terms)
            {
              ub.push_back(make_pair(p.first, exp_ub - p.second));
            }
          }
        }
      assign_ub_finish:

        {
          optional<VariableName> unbounded_lbvar;
          Wt exp_lb(ntov::ntov(exp.constant()));
          vector< pair<VariableName, Wt> > lb_terms;
          for(auto p : exp)
          {
            Wt coeff(ntov::ntov(p.first));
            if(p.first < Wt(0))
            {
              // Again, can't do anything with negative coefficients.
              bound_t y_ub = operator[](p.second.name()).ub();
              if(y_ub.is_infinite())
                goto assign_lb_finish;
              exp_lb += (ntov::ntov(*(y_ub.number())))*coeff;
            } else {
              VariableName y(p.second.name());
              bound_t y_lb = operator[](y).lb(); 
              if(y_lb.is_infinite())
              {
                if(unbounded_lbvar || coeff != Wt(1))
                  goto assign_lb_finish;
                unbounded_lbvar = y;
              } else {
                Wt ymin(ntov::ntov(*(y_lb.number())));
                exp_lb += ymin*coeff;
                lb_terms.push_back(make_pair(y, ymin));
              }
            }
          }

          if(unbounded_lbvar)
          {
            lb.push_back(make_pair(*unbounded_lbvar, exp_lb));
          } else {
            for(auto p : lb_terms)
            {
              lb.push_back(make_pair(p.first, exp_lb - p.second));
            }
          }
        }
    assign_lb_finish:
        return;
      }
   
      // GKG: I suspect there're some sign/bound direction errors in the 
      // following.
      void diffcsts_of_lin_leq(const linear_expression_t& exp, vector<diffcst_t>& csts,
          vector<pair<VariableName, Wt> >& lbs, vector<pair<VariableName, Wt> >& ubs)
      {
        // Process upper bounds.
        Wt unbounded_lbcoeff;
        Wt unbounded_ubcoeff;
        optional<VariableName> unbounded_lbvar;
        optional<VariableName> unbounded_ubvar;
        Wt exp_ub = - (ntov::ntov(exp.constant()));
        vector< pair< pair<Wt, VariableName>, Wt> > pos_terms;
        vector< pair< pair<Wt, VariableName>, Wt> > neg_terms;
        for(auto p : exp)
        {
          Wt coeff(ntov::ntov(p.first));
          if(coeff > Wt(0))
          {
            VariableName y(p.second.name());
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
              pos_terms.push_back(make_pair(make_pair(coeff, y), ymin));
            }
          } else {
            VariableName y(p.second.name());
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
              neg_terms.push_back(make_pair(make_pair(-coeff, y), ymax));
            }
          }
        }

        if(unbounded_lbvar)
        {
          VariableName x(*unbounded_lbvar);
          if(unbounded_ubvar)
          {
            if(unbounded_lbcoeff != Wt(1) || unbounded_ubcoeff != Wt(1))
              goto diffcst_finish;
            VariableName y(*unbounded_ubvar);
            csts.push_back(make_pair(make_pair(x, y), exp_ub));
          } else {
            if(unbounded_lbcoeff == Wt(1))
            {
              for(auto p : neg_terms)
                csts.push_back(make_pair(make_pair(x, p.first.second), exp_ub - p.second));
            }
            // Add bounds for x
            ubs.push_back(make_pair(x, exp_ub/unbounded_lbcoeff));
          }
        } else {
          if(unbounded_ubvar)
          {
            VariableName y(*unbounded_ubvar);
            if(unbounded_ubcoeff == Wt(1))
            {
              for(auto p : pos_terms)
                csts.push_back(make_pair(make_pair(p.first.second, y), exp_ub + p.second));
            }
            // Bounds for y
            lbs.push_back(make_pair(y, -exp_ub/unbounded_ubcoeff));
          } else {
            for(auto pl : neg_terms)
              for(auto pu : pos_terms)
                csts.push_back(make_pair(make_pair(pu.first.second, pl.first.second), exp_ub - pl.second + pu.second));
            for(auto pl : neg_terms)
              lbs.push_back(make_pair(pl.first.second, -exp_ub/pl.first.first + pl.second));
            for(auto pu : pos_terms)
              ubs.push_back(make_pair(pu.first.second, exp_ub/pu.first.first + pu.second));
          }
        }
    diffcst_finish:
        return;
      }

      // Assumption: state is currently feasible.
      void assign(VariableName x, linear_expression_t e) {

        if(is_bottom())
          return;
        CRAB_DEBUG("Before assign: ", *this);
        CRAB_DEBUG(x, ":=", e);
        normalize();

        assert(check_potential(g, potential));

        // If it's a constant, just assign the interval.
        if (e.is_constant()){
          set(x, e.constant());
        } else {
          interval_t x_int = eval_interval(e);
          vector<pair<VariableName, Wt> > diffs_lb;
          vector<pair<VariableName, Wt> > diffs_ub;
          // Construct difference constraints from the assignment
          diffcsts_of_assign(x, e, diffs_lb, diffs_ub);
          if(diffs_lb.size() > 0 || diffs_ub.size() > 0)
          {
            // Allocate a new vertex for x
            vert_id v = g.new_vertex();
            assert(v <= rev_map.size());
            if(v == rev_map.size())
            {
              rev_map.push_back(variable_t(x));
              potential.push_back(potential[0] + eval_expression(e));
            } else {
              potential[v] = potential[0] + eval_expression(e);
              rev_map[v] = x;
            }
            
            edge_vector delta;
            for(auto diff : diffs_lb)
            {
              delta.push_back(make_pair(make_pair(v, get_vert(diff.first)), -diff.second));
            }

            for(auto diff : diffs_ub)
            {
              delta.push_back(make_pair(make_pair(get_vert(diff.first), v), diff.second));
            }
               
            /*
            for(auto diff : delta)
            {
              vert_id s = diff.first.first;
              vert_id d = diff.first.second;

              CRAB_DEBUG("|- ", (*rev_map[d]).name(),"[",d,"]-", (*rev_map[s]).name(), "[", s, "]<=", diff.second);
            }
            */
            GrOps::apply_delta(g, delta);
            delta.clear();
            SubGraph<graph_t> g_excl(g, 0);
            GrOps::close_after_assign(g_excl, potential, v, delta);
            GrOps::apply_delta(g, delta);

            /*
            for(auto diff : delta)
            {
              vert_id s = diff.first.first;
              vert_id d = diff.first.second;

              CRAB_DEBUG("|-> ", (*rev_map[d]).name(),"[",d,"]-", (*rev_map[s]).name(), "[", s, "]<=", diff.second);
            }
            */


            Wt_min min_op;
            if(x_int.lb().is_finite())
              g.update_edge(v, ntov::ntov(-(*(x_int.lb().number()))), 0, min_op);
            if(x_int.ub().is_finite())
              g.update_edge(0, ntov::ntov(*(x_int.ub().number())), v, min_op);
            // Clear the old x vertex
            operator-=(x);
//            ranges.insert(x, x_int);
            vert_map.insert(vmap_elt_t(variable_t(x), v));
          } else {
            set(x, x_int);
          }
          // CRAB_WARN("DBM only supports a cst or var on the rhs of assignment");
          // this->operator-=(x);
        }

//        g.check_adjs(); 

        assert(check_potential(g, potential));
        CRAB_DEBUG("---", x, ":=", e,"\n",*this);
      }

      void apply(operation_t op, VariableName x, VariableName y, VariableName z){	
        if(is_bottom())
          return;

        normalize();

        switch(op)
        {
          case OP_ADDITION:
          {
            linear_expression_t e(linear_expression_t(y) + linear_expression_t(z));
            assign(x, e);
            break;
          }
          case OP_SUBTRACTION:
          {
            linear_expression_t e(linear_expression_t(y) - linear_expression_t(z));
            assign(x, e);
            break;
          }
          // For mul and div, we fall back on intervals.
          case OP_MULTIPLICATION:
          {
            set(x, get_interval(/*ranges,*/y)*get_interval(/*ranges,*/z));
            break;
          }
          case OP_DIVISION:
          {
            interval_t xi(get_interval(/*ranges,*/y)/get_interval(/*ranges,*/z));
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
        CRAB_DEBUG("---", x, ":=", y, op, z,"\n", *this);
      }

    
      void apply(operation_t op, VariableName x, VariableName y, Number k) {	
        if(is_bottom())
          return;

        normalize();

        switch(op)
        {
          case OP_ADDITION:
          {
            linear_expression_t e(linear_expression_t(y) + linear_expression_t(k));
            assign(x, e);
            break;
          }
          case OP_SUBTRACTION:
          {
            linear_expression_t e(linear_expression_t(y) - linear_expression_t(k));
            assign(x, e);
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

        CRAB_DEBUG("---", x, ":=", y, op, k,"\n", *this);
      }
      
      bool add_linear_leq(const linear_expression_t& exp)
      {
        CRAB_DEBUG("Adding: ", exp, "<= 0");
        vector< pair<VariableName, Wt> > lbs;
        vector< pair<VariableName, Wt> > ubs;
        vector<diffcst_t> csts;
        diffcsts_of_lin_leq(exp, csts, lbs, ubs);

        assert(check_potential(g, potential));

        Wt_min min_op;
        for(auto p : lbs)
        {
          CRAB_DEBUG(p.first, ">=", p.second);
          VariableName x(p.first);
          vert_id v = get_vert(p.first);
          if(g.elem(v, 0) && g.edge_val(v, 0) <= -p.second)
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
          for(vert_id s : g.preds(v))
          {
            if(s == 0)
              continue;
            g.update_edge(s, g.edge_val(s, v) - p.second, 0, min_op);
          }
#endif
        }
        for(auto p : ubs)
        {
          CRAB_DEBUG(p.first, "<=", p.second);
          VariableName x(p.first);
          vert_id v = get_vert(p.first);
          if(g.elem(0, v) && g.edge_val(0, v) <= p.second)
            continue;
          g.set_edge(0, p.second, v);
          if(!repair_potential(0, v))
          {
            set_to_bottom();
            return false;
          }
          assert(check_potential(g, potential));

#ifdef CLOSE_BOUNDS_INLINE
          for(vert_id d : g.succs(v))
          {
            if(d == 0)
              continue;
            g.update_edge(0, g.edge_val(v, d) + p.second, d, min_op);
          }
#endif
        }

        for(auto diff : csts)
        {
          CRAB_DEBUG(diff.first.first, "-", diff.first.second, "<=", diff.second);

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
        /* */

        assert(check_potential(g, potential));
        // CRAB_WARN("SplitDBM::add_linear_leq not yet implemented.");
        return true;  
      }
   
      void add_disequation(linear_expression_t exp)
      {
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

      void operator+=(linear_constraint_t cst) {
        if(is_bottom())
          return;
        normalize();

        if (cst.is_tautology())
          return;

//        g.check_adjs();
      
        if (cst.is_contradiction()){
          set_to_bottom();
          return ;
        }

        if (cst.is_inequality())
        {
          if(!add_linear_leq(cst.expression()))
            set_to_bottom();
//          g.check_adjs();
          CRAB_DEBUG("--- ", cst, "\n", *this);
          return;
        }

        if (cst.is_equality())
        {
          linear_expression_t exp = cst.expression();
          if(!add_linear_leq(exp) || !add_linear_leq(-exp))
          {
            CRAB_DEBUG(" ~~> _|_");
            set_to_bottom();
          }
//          g.check_adjs();
          CRAB_DEBUG("--- ", cst, "\n", *this);
          return;
        }

        if (cst.is_disequation())
        {
          add_disequation(cst.expression());
          return;
        }

        CRAB_WARN("Unhandled constraint in SplitDBM");

        CRAB_DEBUG("---", cst, "\n", *this);
        return;
      }
    
      void operator+=(linear_constraint_system_t csts) {  
        if(is_bottom()) return;

        for(auto cst: csts) {
          operator+=(cst);
        }
      }

      // interval_t get_interval(ranges_t& r, variable_t x) {
      interval_t get_interval(variable_t x) {
        return get_interval(vert_map, g, x);
      }
      interval_t get_interval(VariableName x) {
        return get_interval(vert_map, g, x);
      }

      interval_t get_interval(vert_map_t& m, graph_t& r, variable_t x) {
        return get_interval(m, r, x.name());
      }

      // interval_t get_interval(ranges_t& r, VariableName x) {
      interval_t get_interval(vert_map_t& m, graph_t& r, VariableName x) {
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

      interval_t operator[](VariableName x) { 
//        if (is_top())    return interval_t::top();
        if (is_bottom()) return interval_t::bottom();

        if (this->is_bottom()) {
            return interval_t::bottom();
        } else {
          //return get_interval(ranges, x);
          return get_interval(vert_map, g, x);
        }
      }

      void set(VariableName x, interval_t intv) {
        if(is_bottom())
          return;
        if(intv.is_top())
          return;

        this->operator-=(x);
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

      // bitwise_operators_api
      void apply(conv_operation_t op, VariableName x, VariableName y, unsigned width) {
        // since reasoning about infinite precision we simply assign and
        // ignore the width.
        assign(x, linear_expression_t(y));
      }

      void apply(conv_operation_t op, VariableName x, Number k, unsigned width) {
        // since reasoning about infinite precision we simply assign
        // and ignore the width.
        assign(x, k);
      }

      void apply(bitwise_operation_t op, VariableName x, VariableName y, VariableName z) {
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
    
      void apply(bitwise_operation_t op, VariableName x, VariableName y, Number k) {
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
    
      void apply(div_operation_t op, VariableName x, VariableName y, VariableName z) {
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

      void apply(div_operation_t op, VariableName x, VariableName y, Number k) {
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

#ifdef CLOSE_BOUNDS_INLINE
        if(g.elem(0, ii))
          g.update_edge(0, g.edge_val(0, ii) + c, jj, min_op);
        if(g.elem(jj, 0))
          g.update_edge(ii, g.edge_val(jj, 0) + c, 0, min_op);
#endif

        // There may be a cheaper way to do this.
        for(vert_id se : g_excl.preds(ii))
        {
          Wt wt_sij = g_excl.edge_val(se,ii) + c;

          assert(g_excl.succs(se).begin() != g_excl.succs(se).end());
          if(se != jj)
          {
            if(g_excl.elem(se, jj))
            {
              if(g_excl.edge_val(se,jj) <= wt_sij)
                continue;

              g_excl.set_edge(se, wt_sij, jj);
            } else {
              g_excl.add_edge(se, wt_sij, jj);
            }
            
#ifdef CLOSE_BOUNDS_INLINE
            if(g.elem(0, se))
              g.update_edge(0, g.edge_val(0, se) + wt_sij, jj, min_op);
            if(g.elem(jj, 0))
              g.update_edge(se, g.edge_val(jj, 0) + wt_sij, 0, min_op);
#endif

            for(vert_id de : g_excl.succs(jj))
            {
              if(se != de)
              {
                Wt wt_sijd = wt_sij + g_excl.edge_val(jj, de);
                if(g_excl.elem(se, de))
                {
                  if(g_excl.edge_val(se, de) <= wt_sijd)
                    continue;
                  g_excl.set_edge(se, wt_sijd, de);
                } else {
                  g_excl.add_edge(se, wt_sijd, de);
                }
#ifdef CLOSE_BOUNDS_INLINE
                if(g.elem(0, se))
                  g.update_edge(0, g.edge_val(0, se) + wt_sijd, de, min_op);
                if(g.elem(de, 0))
                  g.update_edge(se, g.edge_val(de, 0) + wt_sijd, 0, min_op);
#endif
              }
            }
          }
        }

        for(vert_id de : g_excl.succs(jj))
        {
          Wt wt_ijd = g_excl.edge_val(jj, de) + c;
          if(de != ii)
          {
            if(g_excl.elem(ii, de))
            {
//              g_excl.edge_val(ii, de) = min(g_excl.edge_val(ii, de), wt_ijd);
              if(g_excl.edge_val(ii, de) <= wt_ijd)
                continue;
              g_excl.set_edge(ii, wt_ijd, de);
            } else {
              g_excl.add_edge(ii, wt_ijd, de);
            }
#ifdef CLOSE_BOUNDS_INLINE
            if(g.elem(0,  ii))
              g.update_edge(0, g.edge_val(0, ii) + wt_ijd, de, min_op);
            if(g.elem(de, 0))
              g.update_edge(ii, g.edge_val(de, 0) + wt_ijd, 0, min_op);
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

      //! copy of x into a new fresh variable y
      void expand (VariableName x, VariableName y) {
        if(is_bottom())
          return;

        assign(x, linear_expression_t(y));
//        dbm ret = NULL;      
//        ret = dbm_expand(get_dbm_index(x), get_dbm_index(y), _dbm);
//        dbm_dealloc(_dbm);
//        swap(_dbm, ret);
      }

      // dual of forget: remove all variables except [vIt,...vEt)
      template<typename Iterator>
      void project (Iterator vIt, Iterator vEt) {
        if (is_bottom ())
          return;
        if (vIt == vEt) 
          return;

        normalize();

        vector<bool> save(rev_map.size(), false);
        for(auto x : boost::make_iterator_range(vIt, vEt))
        {
          auto it = vert_map.find(x);
          if(it != vert_map.end())
            save[(*it).second] = true;
        }

        for(vert_id v = 0; v < rev_map.size(); v++)
        {
          if(!save[v] && rev_map[v])
            operator-=((*rev_map[v]).name());
        }
      }

      // Output function
      void write(ostream& o) {

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
            o << "(" << (*(rev_map[v])) << "," << (*(rev_map[d])) << ":" << g.edge_val(v,d) << ")";
          }
        }
        o << "}";
#endif

#if 0
        cout << "var_map={";
        for (auto &p: _var_map) 
          cout << p.first << "(" << p.first.index () << ") " << "->" << p.second << ";";
        cout << "}\n";
        cout << "rev_map={";
        for (auto &p: _rev_map) 
          cout << p.first << "->" << p.second << ";";
        cout << "}\n";
        cout << "matrix:\n";
        dbm_print_to(cout, _dbm);
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
          /*
          for(auto p : ranges)
          {
            if(first)
              first = false;
            else
              o << ", ";
            o << p.first << " -> " << p.second;
          }
          */
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

//          linear_constraint_system_t inv = to_linear_constraint_system ();
//          o << inv;
        }
      }

      linear_constraint_system_t to_linear_constraint_system () {

        normalize ();

        linear_constraint_system_t csts;
    
        if(is_bottom ()) {
          csts += linear_constraint_t (linear_expression_t (Number(1)) == 
                                       linear_expression_t (Number(0)));
          return csts;
        }

        // Extract all the edges

        /* 
        for(auto p : ranges)
        {
          variable_t x = p.first;
          interval_t b = p.second;

          if(b.lb().is_finite())
            csts += linear_constraint_t(linear_expression_t(x) >= linear_expression_t(*(b.lb().number())));
          if(b.ub().is_finite())
            csts += linear_constraint_t(linear_expression_t(x) <= linear_expression_t(*(b.ub().number())));
        }
        */

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
            csts += linear_constraint_t(linear_expression_t(vd) - linear_expression_t(vs) <= linear_expression_t(g_excl.edge_val(s, d)));
          }
        }

        return csts;
      }

      const char* getDomainName () const {return "spDBM";}

    }; // class SplitDBM
  } // namespace domains


  namespace domain_traits {

       using namespace domains;

       template <typename Number, typename VariableName>
       void expand (SplitDBM<Number,VariableName>& inv, 
                    VariableName x, VariableName new_x) {
         inv.expand (x, new_x);
       }
    
       template <typename Number, typename VariableName>
       void normalize (SplitDBM<Number,VariableName>& inv) {
         inv.normalize();
       }
    
       template <typename Number, typename VariableName, typename Iterator >
       void forget (SplitDBM<Number,VariableName>& inv, Iterator it, Iterator end){
         inv.forget (it, end);
       }

       template <typename Number, typename VariableName, typename Iterator >
       void project (SplitDBM<Number,VariableName>& inv, Iterator it, Iterator end) {
         inv.project (it, end);
       }
    } // namespace domain_traits

} // namespace crab


#endif // SPLIT_DBM_HPP
