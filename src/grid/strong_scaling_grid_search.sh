#!/bin/bash

# Strong scaling: fixed input size, varying MPI node/rank count.
# Shared/fixed parameters, including the fixed OpenMP thread count of 16
# for both partition and join, are hardcoded in runners/benchmark_for_scaling.sh.
SCALING_CASES=(
  "MPI_NODES=1 MPI_PROCESSES=1"
  "MPI_NODES=2 MPI_PROCESSES=2"
  "MPI_NODES=4 MPI_PROCESSES=4"
  "MPI_NODES=8 MPI_PROCESSES=8"
)
