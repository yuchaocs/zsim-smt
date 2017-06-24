#configure and build
CONFIGURE = ./configure.sh
SCONS = ${SCONSPATH}/bin/scons
SCONSFLAGS = -j4

# testing
TESTDIR = ./tests/config
TEST = $(TESTDIR)/coarse.cfg
ZSIM = build/opt/zsim

# outputs
BUILDDIR = build/
LIBDIR = lib/
OUTPUT = heartbeat out.cfg *.out *.log.*  *.h5 .scons* tests/traces/* 

build: src/
	$(CONFIGURE)
	$(SCONS) $(SCONSFLAGS)
	# ln -sf ~/zsim/build/opt/zsim ~/opt/bin/zsim

clean:
	$(RM) -rf  $(BUILDDIR)  $(OUTPUT)


test: $(TEST)
	$(ZSIM) $(TEST)	

test-clean:
	$(RM) -rf  $(OUTPUT)

############### unit tests ######################

CC = g++
CPPFLAGS = -Ibuild/opt -g -std=c++11

testArbitration: tests/testArbitration.cpp build/opt/arbitration.o
	$(CC) $(CPPFLAGS) -o $@ $?
