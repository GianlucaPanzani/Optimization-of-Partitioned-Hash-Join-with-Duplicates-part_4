#!/bin/bash
#SBATCH --job-name=hashjoin
#SBATCH --time=00:01:00
#SBATCH --nodes=1
#SBATCH --partition=normal
#SBATCH --output=out/slurm-%j.log
#SBATCH --error=err/slurm-%j.log

set -euo pipefail

if [ "$#" -ne 16 ]; then
    echo "Usage: $0 EXECUTABLE NODE_COUNT NR NS SEED MAX_KEY P DATASET_TYPE PARTITION_THREADS JOIN_THREADS PARTITION_SCHEDULE JOIN_SCHEDULE PARTITION_CHUNK JOIN_CHUNK PARTITION_BLOCK_SIZE MPI_PARTITION_STRATEGY"
    exit 1
fi

EXECUTABLE="$1"
NODE_COUNT="$2"
NR="$3"
NS="$4"
SEED="$5"
MAX_KEY="$6"
P="$7"
DATASET_TYPE="$8"
PARTITION_THREADS="$9"
JOIN_THREADS="${10}"
PARTITION_SCHEDULE="${11}"
JOIN_SCHEDULE="${12}"
PARTITION_CHUNK="${13}"
JOIN_CHUNK="${14}"
PARTITION_BLOCK_SIZE="${15}"
MPI_PARTITION_STRATEGY="${16}"

if ! [[ "$NODE_COUNT" =~ ^[0-9]+$ ]] || [ "$NODE_COUNT" -lt 1 ] || [ "$NODE_COUNT" -gt 8 ]; then
    echo "NODE_COUNT must be an integer in [1, 8], received: $NODE_COUNT"
    exit 1
else
    NODE_COUNT=1
fi

SUBMIT_DIR="${SLURM_SUBMIT_DIR:-$(pwd)}"
cd "$SUBMIT_DIR"

if [[ "$EXECUTABLE" != /* ]]; then
    EXECUTABLE="$SUBMIT_DIR/$EXECUTABLE"
fi

if [ ! -x "$EXECUTABLE" ]; then
    echo "Executable not found or not executable: $EXECUTABLE"
    exit 1
fi

EXECUTABLE_TARGET="$(basename "$EXECUTABLE")"

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
    --mpi-partition-strategy "$MPI_PARTITION_STRATEGY"
)

srun --nodes="$NODE_COUNT" --ntasks="$NODE_COUNT" --ntasks-per-node=1 "${runner_args[@]}"
