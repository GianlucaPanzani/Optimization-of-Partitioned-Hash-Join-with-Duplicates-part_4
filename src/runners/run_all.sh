#!/bin/bash

#./benchmark.sh hashjoin_seq grid/seq_grid_search.sh 5

#./benchmark.sh hashjoin_omp grid/omp_grid_search.sh 5
#./benchmark.sh hashjoin_mpi grid/mpi_grid_search.sh 5
./benchmark.sh hashjoin_hybrid grid/hybrid_grid_search.sh 5

#./benchmark_for_scaling.sh hashjoin_mpi grid/strong_scaling_grid_search.sh 5
#./benchmark_for_scaling.sh hashjoin_mpi grid/weak_scaling_grid_search.sh 5
