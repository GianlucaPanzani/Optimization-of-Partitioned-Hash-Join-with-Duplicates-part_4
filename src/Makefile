CXX            			= g++
MPICXX					= mpicxx
CXXFLAGS       	    	= -std=c++17 -Wall -Wextra -I. -O3
OMPFLAGS				= -fopenmp
AUTOFLAGS      			= -fopt-info-vec-optimized -fopt-info-vec-missed

CHECKER_TARGETS			= checker
SEQ_TARGETS 			= hashjoin_seq
OMP_TARGETS 			= hashjoin_omp
MPI_TARGETS				= hashjoin_mpi
HYBRID_TARGETS			= hashjoin_hybrid
SCALING_TARGETS			= strong_scaling weak_scaling
EXECUTABLE_TARGETS		= $(CHECKER_TARGETS) $(SEQ_TARGETS) $(OMP_TARGETS) $(MPI_TARGETS) $(HYBRID_TARGETS) $(SCALING_TARGETS)

GCC_SEQ_REPORT  			= compilation/gcc_seq_report.txt
GCC_OMP_REPORT				= compilation/gcc_omp_report.txt
GCC_MPI_REPORT				= compilation/gcc_mpi_report.txt
GCC_HYBRID_REPORT			= compilation/gcc_hybrid_report.txt
GCC_STRONG_SCALING_REPORT	= compilation/gcc_strong_scaling_report.txt
GCC_WEAK_SCALING_REPORT		= compilation/gcc_weak_scaling_report.txt
GCC_CHECKER_REPORT			= compilation/gcc_checker_report.txt
GCC_REPORTS					= compilation/gcc_*_report.txt

RES_SEQ					= results/hashjoin_seq.csv
RES_OMP					= results/hashjoin_omp.csv
RES_MPI					= results/hashjoin_mpi.csv
RES_HYBRID				= results/hashjoin_hybrid.csv
RES_STRONG_SCALING		= results/strong_scaling.csv
RES_WEAK_SCALING		= results/weak_scaling.csv
RESULTS_FILES			= results/*.csv

LIB_SOURCES    			= lib/timing.cpp lib/results.cpp
LOG_FILES				= out/*.log err/*.log


.PHONY: all checker clean cleanlogs cleanall_seq cleanall_omp cleanall_mpi cleanall_hybrid cleanall_strong_scaling cleanall_weak_scaling cleanall

all: $(EXECUTABLE_TARGETS)


checker: checker.cpp
	@mkdir -p compilation
	$(CXX) $(CXXFLAGS) -o $@ checker.cpp 2> $(GCC_CHECKER_REPORT)


hashjoin_seq: hashjoin_seq.cpp $(LIB_SOURCES)
	@mkdir -p compilation
	$(CXX) $(CXXFLAGS) $(AUTOFLAGS) -o $@ hashjoin_seq.cpp $(LIB_SOURCES) 2> $(GCC_SEQ_REPORT)

hashjoin_omp: hashjoin_omp.cpp $(LIB_SOURCES)
	@mkdir -p compilation
	$(CXX) $(CXXFLAGS) $(OMPFLAGS) $(AUTOFLAGS) -o $@ hashjoin_omp.cpp $(LIB_SOURCES) 2> $(GCC_OMP_REPORT)

hashjoin_mpi: hashjoin_mpi.cpp $(LIB_SOURCES)
	@mkdir -p compilation
	$(MPICXX) $(CXXFLAGS) $(AUTOFLAGS) -o $@ hashjoin_mpi.cpp $(LIB_SOURCES) 2> $(GCC_MPI_REPORT)

hashjoin_hybrid: hashjoin_hybrid.cpp $(LIB_SOURCES)
	@mkdir -p compilation
	$(MPICXX) $(CXXFLAGS) $(OMPFLAGS) $(AUTOFLAGS) -o $@ hashjoin_hybrid.cpp $(LIB_SOURCES) 2> $(GCC_HYBRID_REPORT)

strong_scaling: strong_scaling.cpp $(LIB_SOURCES)
	@mkdir -p compilation
	$(CXX) $(CXXFLAGS) $(OMPFLAGS) $(AUTOFLAGS) -o $@ strong_scaling.cpp $(LIB_SOURCES) 2> $(GCC_STRONG_SCALING_REPORT)

weak_scaling: weak_scaling.cpp $(LIB_SOURCES)
	@mkdir -p compilation
	$(CXX) $(CXXFLAGS) $(OMPFLAGS) $(AUTOFLAGS) -o $@ weak_scaling.cpp $(LIB_SOURCES) 2> $(GCC_WEAK_SCALING_REPORT)


clean:
	-rm -fr *.o *~ $(EXECUTABLE_TARGETS)

cleanlogs: clean
	-rm -fr $(LOG_FILES)

cleanall_seq: clean
	-rm -fr hashjoin_seq $(GCC_SEQ_REPORT) $(LOG_FILES) $(RES_SEQ)

cleanall_omp: clean
	-rm -fr hashjoin_omp $(GCC_OMP_REPORT) $(LOG_FILES) $(RES_OMP)

cleanall_mpi: clean
	-rm -fr hashjoin_mpi $(GCC_MPI_REPORT) $(LOG_FILES) $(RES_MPI)

cleanall_hybrid: clean
	-rm -fr hashjoin_hybrid $(GCC_HYBRID_REPORT) $(LOG_FILES) $(RES_HYBRID)

cleanall_strong_scaling: clean
	-rm -fr strong_scaling $(GCC_STRONG_SCALING_REPORT) $(LOG_FILES) $(RES_STRONG_SCALING)

cleanall_weak_scaling: clean
	-rm -fr weak_scaling $(GCC_WEAK_SCALING_REPORT) $(LOG_FILES) $(RES_WEAK_SCALING)

cleanall: clean
	-rm -fr $(EXECUTABLE_TARGETS) $(GCC_REPORTS) $(LOG_FILES) $(RESULTS_FILES)
