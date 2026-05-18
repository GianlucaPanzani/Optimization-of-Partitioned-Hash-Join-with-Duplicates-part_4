#!/bin/bash

# Static parameters
N_VALUES=(50000000)
P_VALUES=(256)
SEED_VALUES=(13)
MAX_KEY_VALUES=(1000000)

# --- Full OMP combinations ---
# Dataset distributions type (e.g. skewed_80_5 means 80% of records go to 5% of the partitions)
DATASET_TYPE_VALUES=(uniform skewed_90_5 skewed_90_1)
# OpenMP thread configurations
PARTITION_THREAD_VALUES=(16 32 64)
JOIN_THREAD_VALUES=(16 32 64)
# Supported by hashjoin_omp_loop.cpp: static | dynamic | guided | auto
PARTITION_SCHEDULE_VALUES=(static dynamic guided)
JOIN_SCHEDULE_VALUES=(static dynamic guided)
# Use 0 to mean "no explicit chunk" (e.g. schedule(static) instead of schedule(static,chunk))
PARTITION_CHUNK_VALUES=(0 32)
JOIN_CHUNK_VALUES=(0 32)
# Block size used by the parallel partitioning implementation (for the block-based histogram/scatter phase)
PARTITION_BLOCK_SIZE_VALUES=(32768 131072)

# --- Reduced OMP combinations ---
# OpenMP thread configurations
PARTITION_THREAD_VALUES=(32)
JOIN_THREAD_VALUES=(32)
# Supported by hashjoin_omp_loop.cpp: static | dynamic | guided | auto
PARTITION_SCHEDULE_VALUES=(guided)
JOIN_SCHEDULE_VALUES=(guided)
# Use 0 to mean "no explicit chunk" (e.g. schedule(static) instead of schedule(static,chunk))
PARTITION_CHUNK_VALUES=(4 8)
JOIN_CHUNK_VALUES=(4 8)
# Block size used by the parallel partitioning implementation (for the block-based histogram/scatter phase)
PARTITION_BLOCK_SIZE_VALUES=(32768)


# --- Unused parameters for the OMP loop version ---
PARTITION_TASK_GRAIN_VALUES=(1)
JOIN_TASK_GRAIN_VALUES=(1)
OFFSET_TASK_GRAIN_VALUES=(1)