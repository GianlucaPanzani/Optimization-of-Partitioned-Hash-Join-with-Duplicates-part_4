#!/bin/bash

./benchmark.sh hashjoin_seq grid/seq_grid_search.sh 5

./benchmark.sh hashjoin_omp_loop grid/omp_loop_grid_search.sh 5
./benchmark.sh hashjoin_omp_loop_wb grid/omp_loop_grid_search.sh 5

./benchmark.sh hashjoin_omp_task grid/omp_task_grid_search.sh 5
./benchmark.sh hashjoin_omp_task_wb grid/omp_task_grid_search.sh 5

./benchmark.sh hashjoin_omp_taskloop grid/omp_taskloop_grid_search.sh 5
./benchmark.sh hashjoin_omp_taskloop_wb grid/omp_taskloop_grid_search.sh 5

./run_weak_scaling.sh
./run_strong_scaling.sh

