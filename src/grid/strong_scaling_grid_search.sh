#!/bin/bash

# Strong scaling: fixed input size, varying thread count.
# Shared/fixed parameters and dataset types are hardcoded in
# runners/benchmark_for_scaling.sh.
SCALING_CASES=(
  "PARTITION_THREADS=1 JOIN_THREADS=1"
  "PARTITION_THREADS=2 JOIN_THREADS=2"
  "PARTITION_THREADS=4 JOIN_THREADS=4"
  "PARTITION_THREADS=8 JOIN_THREADS=8"
  "PARTITION_THREADS=16 JOIN_THREADS=16"
  "PARTITION_THREADS=32 JOIN_THREADS=32"
  "PARTITION_THREADS=64 JOIN_THREADS=64"
)
