#!/bin/bash
#SBATCH --job-name=hashjoin
#SBATCH --time=00:01:00
#SBATCH --nodes=1
#SBATCH --partition=normal
#SBATCH --output=out/slurm-%j.log
#SBATCH --error=err/slurm-%j.log

set -euo pipefail

if [ "$#" -lt 17 ] || [ "$#" -gt 18 ]; then
    echo "Usage: $0 EXECUTABLE NODE_COUNT MPI_PROCESS_COUNT NR NS SEED MAX_KEY P DATASET_TYPE PARTITION_THREADS JOIN_THREADS PARTITION_SCHEDULE JOIN_SCHEDULE PARTITION_CHUNK JOIN_CHUNK PARTITION_BLOCK_SIZE MPI_PARTITION_STRATEGY [OUTPUT_CSV]"
    exit 1
fi

EXECUTABLE="$1"
NODE_COUNT="$2"
MPI_PROCESS_COUNT="$3"
NR="$4"
NS="$5"
SEED="$6"
MAX_KEY="$7"
P="$8"
DATASET_TYPE="$9"
PARTITION_THREADS="${10}"
JOIN_THREADS="${11}"
PARTITION_SCHEDULE="${12}"
JOIN_SCHEDULE="${13}"
PARTITION_CHUNK="${14}"
JOIN_CHUNK="${15}"
PARTITION_BLOCK_SIZE="${16}"
MPI_PARTITION_STRATEGY="${17}"
OUTPUT_CSV="${18:-}"

if ! [[ "$NODE_COUNT" =~ ^[0-9]+$ ]] || [ "$NODE_COUNT" -lt 1 ] || [ "$NODE_COUNT" -gt 8 ]; then
    echo "NODE_COUNT must be an integer in [1, 8], received: $NODE_COUNT"
    exit 1
fi

if ! [[ "$MPI_PROCESS_COUNT" =~ ^[0-9]+$ ]] || [ "$MPI_PROCESS_COUNT" -lt 1 ]; then
    echo "MPI_PROCESS_COUNT must be a positive integer, received: $MPI_PROCESS_COUNT"
    exit 1
fi

if [ "$MPI_PROCESS_COUNT" -lt "$NODE_COUNT" ]; then
    echo "MPI_PROCESS_COUNT must be >= NODE_COUNT, received nodes=$NODE_COUNT processes=$MPI_PROCESS_COUNT"
    exit 1
fi

if [ $((MPI_PROCESS_COUNT % NODE_COUNT)) -ne 0 ]; then
    echo "MPI_PROCESS_COUNT must be divisible by NODE_COUNT to use a fixed --ntasks-per-node layout, received nodes=$NODE_COUNT processes=$MPI_PROCESS_COUNT"
    exit 1
fi

TASKS_PER_NODE=$((MPI_PROCESS_COUNT / NODE_COUNT))

SUBMIT_DIR="${SLURM_SUBMIT_DIR:-$(pwd)}"
cd "$SUBMIT_DIR"

if [[ "$EXECUTABLE" != /* ]]; then
    EXECUTABLE="$SUBMIT_DIR/$EXECUTABLE"
fi

if [ ! -x "$EXECUTABLE" ]; then
    echo "Executable not found or not executable: $EXECUTABLE"
    exit 1
fi

MAX_THREADS="$PARTITION_THREADS"
if [ "$JOIN_THREADS" -gt "$MAX_THREADS" ]; then
    MAX_THREADS="$JOIN_THREADS"
fi

export OMP_NUM_THREADS="$MAX_THREADS"
export OMP_DISPLAY_ENV="${OMP_DISPLAY_ENV:-false}"

runner_args=(
    "$EXECUTABLE"
    -nr "$NR"
    -ns "$NS"
    -seed "$SEED"
    -max-key "$MAX_KEY"
    -p "$P"
    --dataset-type "$DATASET_TYPE"
    --partition-threads "$PARTITION_THREADS"
    --join-threads "$JOIN_THREADS"
    --partition-schedule "$PARTITION_SCHEDULE"
    --join-schedule "$JOIN_SCHEDULE"
    --partition-chunk "$PARTITION_CHUNK"
    --join-chunk "$JOIN_CHUNK"
    --partition-block-size "$PARTITION_BLOCK_SIZE"
    --mpi-nodes "$NODE_COUNT"
    --mpi-processes "$MPI_PROCESS_COUNT"
    --mpi-partition-strategy "$MPI_PARTITION_STRATEGY"
)

if [ -n "$OUTPUT_CSV" ]; then
    runner_args+=(--output-csv "$OUTPUT_CSV")
fi

srun --mpi=pmix --nodes="$NODE_COUNT" --ntasks="$MPI_PROCESS_COUNT" --ntasks-per-node="$TASKS_PER_NODE" "${runner_args[@]}"
