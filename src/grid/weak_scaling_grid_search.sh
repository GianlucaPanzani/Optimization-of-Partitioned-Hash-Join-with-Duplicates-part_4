#!/bin/bash

# Weak scaling: input size grows with the MPI node/rank count.
# Shared/fixed parameters, including the fixed OpenMP thread count of 16
# for both partition and join, are hardcoded in runners/benchmark_for_scaling.sh.
SCALING_CASES=(
  "NR=10000000 NS=10000000 MPI_NODES=1 MPI_PROCESSES=1"
  "NR=20000000 NS=20000000 MPI_NODES=2 MPI_PROCESSES=2"
  "NR=40000000 NS=40000000 MPI_NODES=4 MPI_PROCESSES=4"
  "NR=80000000 NS=80000000 MPI_NODES=8 MPI_PROCESSES=8"
)
