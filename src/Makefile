CXX            			= g++
CXXFLAGS       	    	= -std=c++17 -Wall -Wextra -I. -O3
OMPFLAGS				= -fopenmp
AUTOFLAGS      			= -fopt-info-vec-optimized -fopt-info-vec-missed

CHECKER_TARGETS			= checker
SEQ_TARGETS 			= hashjoin_seq
OMP_LOOP_TARGETS 		= hashjoin_omp_loop hashjoin_omp_loop_wb
OMP_TASK_TARGETS 		= hashjoin_omp_task hashjoin_omp_task_wb hashjoin_omp_taskloop hashjoin_omp_taskloop_wb
SCALING_TARGETS			= strong_scaling weak_scaling
EXECUTABLE_TARGETS		= $(CHECKER_TARGETS) $(SEQ_TARGETS) $(OMP_LOOP_TARGETS) $(OMP_TASK_TARGETS) $(SCALING_TARGETS)

GCC_SEQ_REPORT  			= compilation/gcc_seq_report.txt
GCC_OMP_LOOP_REPORT			= compilation/gcc_omp_loop_report.txt
GCC_OMP_LOOP_WB_REPORT		= compilation/gcc_omp_loop_wb_report.txt
GCC_OMP_TASK_REPORT			= compilation/gcc_omp_task_report.txt
GCC_OMP_TASK_WB_REPORT		= compilation/gcc_omp_task_wb_report.txt
GCC_OMP_TASKLOOP_REPORT		= compilation/gcc_omp_taskloop_report.txt
GCC_OMP_TASKLOOP_WB_REPORT	= compilation/gcc_omp_taskloop_wb_report.txt
GCC_STRONG_SCALING_REPORT	= compilation/gcc_strong_scaling_report.txt
GCC_WEAK_SCALING_REPORT		= compilation/gcc_weak_scaling_report.txt
GCC_CHECKER_REPORT			= compilation/gcc_checker_report.txt
GCC_REPORTS					= compilation/gcc_*_report.txt

RES_SEQ					= results/hashjoin_seq.csv
RES_OMP_LOOP			= results/hashjoin_omp_loop.csv
RES_OMP_LOOP_WB			= results/hashjoin_omp_loop_wb.csv
RES_OMP_TASK			= results/hashjoin_omp_task.csv
RES_OMP_TASK_WB			= results/hashjoin_omp_task_wb.csv
RES_OMP_TASKLOOP		= results/hashjoin_omp_taskloop.csv
RES_OMP_TASKLOOP_WB		= results/hashjoin_omp_taskloop_wb.csv
RES_STRONG_SCALING		= results/strong_scaling.csv
RES_WEAK_SCALING		= results/weak_scaling.csv
RESULTS_FILES			= results/*.csv

LIB_SOURCES    			= lib/timing.cpp lib/results.cpp
LOG_FILES				= out/*.log err/*.log


.PHONY: all checker clean cleanlogs cleanall_seq cleanall_omp_loop cleanall_omp_loop_wb cleanall_omp_task cleanall_omp_task_wb cleanall_omp_taskloop cleanall_omp_taskloop_wb cleanall_strong_scaling cleanall_weak_scaling cleanall

all: $(EXECUTABLE_TARGETS)


checker: checker.cpp
	$(CXX) $(CXXFLAGS) -o $@ checker.cpp 2> $(GCC_CHECKER_REPORT)


hashjoin_seq: hashjoin_seq.cpp $(LIB_SOURCES)
	$(CXX) $(CXXFLAGS) $(AUTOFLAGS) -o $@ hashjoin_seq.cpp $(LIB_SOURCES) 2> $(GCC_SEQ_REPORT)

hashjoin_omp_loop: hashjoin_omp_loop.cpp $(LIB_SOURCES)
	$(CXX) $(CXXFLAGS) $(OMPFLAGS) $(AUTOFLAGS) -o $@ hashjoin_omp_loop.cpp $(LIB_SOURCES) 2> $(GCC_OMP_LOOP_REPORT)

hashjoin_omp_loop_wb: hashjoin_omp_loop_wb.cpp $(LIB_SOURCES)
	$(CXX) $(CXXFLAGS) $(OMPFLAGS) $(AUTOFLAGS) -o $@ hashjoin_omp_loop_wb.cpp $(LIB_SOURCES) 2> $(GCC_OMP_LOOP_WB_REPORT)

hashjoin_omp_task: hashjoin_omp_task.cpp $(LIB_SOURCES)
	$(CXX) $(CXXFLAGS) $(OMPFLAGS) $(AUTOFLAGS) -o $@ hashjoin_omp_task.cpp $(LIB_SOURCES) 2> $(GCC_OMP_TASK_REPORT)

hashjoin_omp_task_wb: hashjoin_omp_task_wb.cpp $(LIB_SOURCES)
	$(CXX) $(CXXFLAGS) $(OMPFLAGS) $(AUTOFLAGS) -o $@ hashjoin_omp_task_wb.cpp $(LIB_SOURCES) 2> $(GCC_OMP_TASK_WB_REPORT)

hashjoin_omp_taskloop: hashjoin_omp_taskloop.cpp $(LIB_SOURCES)
	$(CXX) $(CXXFLAGS) $(OMPFLAGS) $(AUTOFLAGS) -o $@ hashjoin_omp_taskloop.cpp $(LIB_SOURCES) 2> $(GCC_OMP_TASKLOOP_REPORT)

hashjoin_omp_taskloop_wb: hashjoin_omp_taskloop_wb.cpp $(LIB_SOURCES)
	$(CXX) $(CXXFLAGS) $(OMPFLAGS) $(AUTOFLAGS) -o $@ hashjoin_omp_taskloop_wb.cpp $(LIB_SOURCES) 2> $(GCC_OMP_TASKLOOP_WB_REPORT)

strong_scaling: strong_scaling.cpp $(LIB_SOURCES)
	$(CXX) $(CXXFLAGS) $(OMPFLAGS) $(AUTOFLAGS) -o $@ strong_scaling.cpp $(LIB_SOURCES) 2> $(GCC_STRONG_SCALING_REPORT)

weak_scaling: weak_scaling.cpp $(LIB_SOURCES)
	$(CXX) $(CXXFLAGS) $(OMPFLAGS) $(AUTOFLAGS) -o $@ weak_scaling.cpp $(LIB_SOURCES) 2> $(GCC_WEAK_SCALING_REPORT)


clean:
	-rm -fr *.o *~ $(EXECUTABLE_TARGETS)

cleanlogs: clean
	-rm -fr $(LOG_FILES)

cleanall_seq: clean
	-rm -fr hashjoin_seq $(GCC_SEQ_REPORT) $(LOG_FILES) $(RES_SEQ)

cleanall_omp_loop: clean
	-rm -fr hashjoin_omp_loop $(GCC_OMP_LOOP_REPORT) $(LOG_FILES) $(RES_OMP_LOOP)

cleanall_omp_loop_wb: clean
	-rm -fr hashjoin_omp_loop_wb $(GCC_OMP_LOOP_WB_REPORT) $(LOG_FILES) $(RES_OMP_LOOP_WB)

cleanall_omp_task: clean
	-rm -fr hashjoin_omp_task $(GCC_OMP_TASK_REPORT) $(LOG_FILES) $(RES_OMP_TASK)

cleanall_omp_task_wb: clean
	-rm -fr hashjoin_omp_task_wb $(GCC_OMP_TASK_WB_REPORT) $(LOG_FILES) $(RES_OMP_TASK_WB)

cleanall_omp_taskloop: clean
	-rm -fr hashjoin_omp_taskloop $(GCC_OMP_TASKLOOP_REPORT) $(LOG_FILES) $(RES_OMP_TASKLOOP)

cleanall_omp_taskloop_wb: clean
	-rm -fr hashjoin_omp_taskloop_wb $(GCC_OMP_TASKLOOP_WB_REPORT) $(LOG_FILES) $(RES_OMP_TASKLOOP_WB)

cleanall_strong_scaling: clean
	-rm -fr strong_scaling $(GCC_STRONG_SCALING_REPORT) $(LOG_FILES) $(RES_STRONG_SCALING)

cleanall_weak_scaling: clean
	-rm -fr weak_scaling $(GCC_WEAK_SCALING_REPORT) $(LOG_FILES) $(RES_WEAK_SCALING)

cleanall: clean
	-rm -fr $(EXECUTABLE_TARGETS) $(GCC_REPORTS) $(LOG_FILES) $(RESULTS_FILES)
