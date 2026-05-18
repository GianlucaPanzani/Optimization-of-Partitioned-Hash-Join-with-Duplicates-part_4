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
PARTITION_THREAD_VALUES=(8 16 32 64)
JOIN_THREAD_VALUES=(8 16 32 64)
# Block size used by the parallel partitioning implementation (for the block-based histogram/scatter phase)
PARTITION_BLOCK_SIZE_VALUES=(32768 131072)
# Taskloop grains (partition grain measured in input blocks, join/offset grains measured in partitions)
PARTITION_TASKLOOP_GRAIN_VALUES=(1 32)
JOIN_TASKLOOP_GRAIN_VALUES=(1 32)
OFFSET_TASKLOOP_GRAIN_VALUES=(1)

# --- Reduced OMP combinations ---
# OpenMP thread configurations
PARTITION_THREAD_VALUES=(64)
JOIN_THREAD_VALUES=(32)
# Block size used by the parallel partitioning implementation (for the block-based histogram/scatter phase)
PARTITION_BLOCK_SIZE_VALUES=(32768)
# Taskloop grains (partition grain measured in input blocks, join/offset grains measured in partitions)
PARTITION_TASKLOOP_GRAIN_VALUES=(2 4)
JOIN_TASKLOOP_GRAIN_VALUES=(4 8)
OFFSET_TASKLOOP_GRAIN_VALUES=(2)

# --- Unused parameters for the OMP taskloop version ---
PARTITION_SCHEDULE_VALUES=(auto)
JOIN_SCHEDULE_VALUES=(auto)
PARTITION_CHUNK_VALUES=(0)
JOIN_CHUNK_VALUES=(0)
