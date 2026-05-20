#!/bin/bash

#./benchmark.sh hashjoin_seq grid/seq_grid_search.sh 5

#./benchmark.sh hashjoin_omp grid/omp_grid_search.sh 5
./benchmark.sh hashjoin_mpi grid/mpi_grid_search.sh 1
#./benchmark.sh hashjoin_hybrid grid/hybrid_grid_search.sh 1

#./run_weak_scaling.sh
#./run_strong_scaling.sh
