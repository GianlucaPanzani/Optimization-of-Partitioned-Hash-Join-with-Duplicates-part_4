#!/bin/bash

# Static parameters
NODE_VALUES=(1)
MPI_PROCESS_VALUES=(1)
N_VALUES=(50000000)
P_VALUES=(256)
SEED_VALUES=(13)
MAX_KEY_VALUES=(1000000)

# --- Full OMP combinations ---
# Dataset distributions type (e.g. skewed_80_5 means 80% of records go to 5% of the partitions)
DATASET_TYPE_VALUES=(uniform skewed_90_5 skewed_90_1)
# OpenMP thread configurations
PARTITION_THREAD_VALUES=(32)
JOIN_THREAD_VALUES=(32)
# Supported by hashjoin_omp.cpp: static | dynamic | guided | auto
PARTITION_SCHEDULE_VALUES=(guided)
JOIN_SCHEDULE_VALUES=(guided)
# Use 0 to mean "no explicit chunk" (e.g. schedule(static) instead of schedule(static,chunk))
PARTITION_CHUNK_VALUES=(4 8)
JOIN_CHUNK_VALUES=(4 8)
# Block size used by the parallel partitioning implementation (for the block-based histogram/scatter phase)
PARTITION_BLOCK_SIZE_VALUES=(32768)


# --- Unused MPI parameter for the OMP version ---
MPI_PARTITION_STRATEGY_VALUES=(block)
