#------------------------------------------------------------------------------------#
#                                  INSTRUCTIONS
#------------------------------------------------------------------------------------#
#  1. Set CRAB_INSTALL to the directory Crab was installed. If you
#  have not installed Crab yet, then type `make crab_install`
#  2. Type `make all`
#  3. Type `./test`
#------------------------------------------------------------------------------------#

CRAB_INSTALL := ${HOME}/Repos/crab/install

UNAME := $(shell uname)
ifeq ($(UNAME),Darwin)
    CRAB_LIBS = $(CRAB_INSTALL)/crab/lib/libCrab.dylib
else
    CRAB_LIBS = $(CRAB_INSTALL)/crab/lib/libCrab.so
endif

# Lookup path for libCrab.so
LDFLAGS := -Wl,-rpath,$(CRAB_INSTALL)/lib/

LDD_LIBS := \
    $(CRAB_INSTALL)/ldd/lib/libldd.a \
    $(CRAB_INSTALL)/ldd/lib/libtvpi.a \
    $(CRAB_INSTALL)/ldd/lib/libcudd.a \
    $(CRAB_INSTALL)/ldd/lib/libst.a \
    $(CRAB_INSTALL)/ldd/lib/libutil.a \
    $(CRAB_INSTALL)/ldd/lib/libmtr.a \
    $(CRAB_INSTALL)/ldd/lib/libepd.a 

APRON_LIBS := \
    $(CRAB_INSTALL)/apron/lib/libpolkaMPQ.a \
    $(CRAB_INSTALL)/apron/lib/liboctD.a \
    $(CRAB_INSTALL)/apron/lib/liboptoct.a \
    $(CRAB_INSTALL)/apron/lib/liblinkedlistapi.a \
    $(CRAB_INSTALL)/apron/lib/libapron.a \
    $(CRAB_INSTALL)/apron/lib/libboxMPQ.a \
    $(CRAB_INSTALL)/apron/lib/libitvMPQ.a

LDLIBS := \
    $(CRAB_LIBS) $(LDD_LIBS) $(APRON_LIBS) \
    -lmpfr -lgmpxx -lgmp -lm -lstdc++ 


CXXFLAGS := \
    -Wall -Werror -Wfatal-errors \
    -Wno-unused-local-typedefs -Wno-inconsistent-missing-override -Wno-unused-const-variable \
    -Wno-uninitialized -Wno-deprecated \
    -DBSD -DHAVE_IEEE_754 -DSIZEOF_VOID_P=8 -DSIZEOF_LONG=8 \
    -I $(CRAB_INSTALL)/crab/include \
    -I $(CRAB_INSTALL)/ldd/include/ldd \
    -I $(CRAB_INSTALL)/apron/include \
    -O2 -g -std=c++11

all: test

SOURCES = $(wildcard *.cpp)
C_SOURCES += $(wildcard *.c)

OBJECTS = $(patsubst %.cpp,%.o,${SOURCES})
OBJECTS += $(patsubst %.c,%.o,${C_SOURCES})

%.o:  $(CXX) $(CXXFLAGS) $< -c -o $@

test: ${OBJECTS}
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(OBJECTS) $(LDLIBS) -o test 

clean:
	rm -f test *.a *.o

crab_install:
	git clone https://github.com/seahorn/crab.git crab
	mkdir -p crab/build
	cd crab/build \
	    && cmake -DCMAKE_INSTALL_PREFIX=$(CRAB_INSTALL) -DUSE_LDD=ON -DUSE_APRON=ON ../ \
	    && cmake --build . --target ldd && cmake ../ \
	    && cmake --build . --target apron && cmake ../ \
	    && cmake --build . --target install