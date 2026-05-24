#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXE_DIR="$SCRIPT_DIR/.."
RUNNER_SCRIPT="$SCRIPT_DIR/run_with_slurm.sh"

cd "$EXE_DIR"
mkdir -p out err

if [ "$#" -lt 1 ] || [ "$#" -gt 3 ]; then
    echo "Usage: $0 EXECUTABLE_NAME [GRID_CONFIG(optional)] [REPEAT_COUNT(optional)]"
    echo "  EXECUTABLE_NAME: make target to compile and benchmark"
    echo "  GRID_CONFIG: shell file exporting benchmark arrays"
    echo "  REPEAT_COUNT: positive integer (default: 1)"
    exit 1
fi

EXECUTABLE_INPUT="$1"
EXECUTABLE_TARGET="$(basename "$EXECUTABLE_INPUT")"
MAKE_TARGET="$EXECUTABLE_TARGET"
GRID_CONFIG="$EXE_DIR/grid/omp_grid_search.sh"
REPEAT_COUNT="1"

if [ $# -eq 2 ]; then
    if [[ "$2" =~ ^[0-9]+$ ]]; then
        REPEAT_COUNT="$2"
    else
        if [[ "$2" = /* ]]; then
            GRID_CONFIG="$2"
        else
            GRID_CONFIG="$EXE_DIR/$2"
        fi
    fi
elif [ $# -eq 3 ]; then
    if [[ "$2" = /* ]]; then
        GRID_CONFIG="$2"
    else
        GRID_CONFIG="$EXE_DIR/$2"
    fi
    REPEAT_COUNT="$3"
fi

if ! [[ "$REPEAT_COUNT" =~ ^[0-9]+$ ]] || [ "$REPEAT_COUNT" -lt 1 ]; then
    echo "REPEAT_COUNT must be a positive integer, received: $REPEAT_COUNT"
    exit 1
fi

if [ ! -f "$GRID_CONFIG" ]; then
    echo "Grid configuration file not found: $GRID_CONFIG"
    exit 1
fi

if [ ! -f "$RUNNER_SCRIPT" ]; then
    echo "Runner script not found: $RUNNER_SCRIPT"
    exit 1
fi

source "$GRID_CONFIG"

require_non_empty_array() {
    local array_name="$1"
    local array_len
    local declaration

    if [[ ! "$array_name" =~ ^[a-zA-Z_][a-zA-Z0-9_]*$ ]]; then
        return 1
    fi

    if ! declaration="$(declare -p "$array_name" 2>/dev/null)"; then
        return 1
    fi

    if [[ "$declaration" != declare\ -a* && "$declaration" != declare\ -A* ]]; then
        return 1
    fi

    eval "array_len=\${#$array_name[@]}"
    [ "$array_len" -gt 0 ]
}

valid_mpi_layout() {
    local nodes="$1"
    local processes="$2"
    [[ "$nodes" =~ ^[0-9]+$ ]] || return 1
    [[ "$processes" =~ ^[0-9]+$ ]] || return 1
    [ "$nodes" -ge 1 ] || return 1
    [ "$processes" -ge "$nodes" ] || return 1
    [ $((processes % nodes)) -eq 0 ] || return 1
}

REQUIRED_ARRAYS=(
    NODE_VALUES
    MPI_PROCESS_VALUES
    N_VALUES
    P_VALUES
    SEED_VALUES
    MAX_KEY_VALUES
    DATASET_TYPE_VALUES
    PARTITION_THREAD_VALUES
    JOIN_THREAD_VALUES
    PARTITION_SCHEDULE_VALUES
    JOIN_SCHEDULE_VALUES
    PARTITION_CHUNK_VALUES
    JOIN_CHUNK_VALUES
    PARTITION_BLOCK_SIZE_VALUES
    MPI_PARTITION_STRATEGY_VALUES
)

for ARRAY_NAME in "${REQUIRED_ARRAYS[@]}"; do
    if ! require_non_empty_array "$ARRAY_NAME"; then
        echo "Grid configuration must define non-empty array: $ARRAY_NAME"
        exit 1
    fi
done

case "$EXECUTABLE_TARGET" in
    hashjoin_seq)
        make cleanall_seq
        ;;
    hashjoin_omp)
        make cleanall_omp
        ;;
    hashjoin_mpi)
        make cleanall_mpi
        ;;
    hashjoin_hybrid|hashjoin_hybrid_uniform|hashjoin_hybrid_skew1|hashjoin_hybrid_skew2)
        MAKE_TARGET="hashjoin_hybrid"
        make cleanall_hybrid
        ;;
    *)
        echo "Unknown executable target: $EXECUTABLE_TARGET"
        echo "Add the target to the Makefile and to benchmark.sh if needed."
        exit 1
        ;;
esac

mkdir -p compilation

if ! make -B "$MAKE_TARGET"; then
    echo "Compilation failed or unknown make target: $MAKE_TARGET"
    exit 1
fi

EXECUTABLE="$EXE_DIR/$MAKE_TARGET"

if [ ! -x "$EXECUTABLE" ]; then
    echo "Compiled executable not found or not executable: $EXECUTABLE"
    exit 1
fi

VALID_MPI_LAYOUTS=0
for NODE_COUNT in "${NODE_VALUES[@]}"; do
    for MPI_PROCESS_COUNT in "${MPI_PROCESS_VALUES[@]}"; do
        if valid_mpi_layout "$NODE_COUNT" "$MPI_PROCESS_COUNT"; then
            VALID_MPI_LAYOUTS=$((VALID_MPI_LAYOUTS + 1))
        fi
    done
done

if [ "$VALID_MPI_LAYOUTS" -eq 0 ]; then
    echo "No valid MPI layouts. Require processes >= nodes and processes divisible by nodes."
    exit 1
fi

TOTAL=$(( \
    VALID_MPI_LAYOUTS * \
    ${#N_VALUES[@]} * \
    ${#P_VALUES[@]} * \
    ${#SEED_VALUES[@]} * \
    ${#MAX_KEY_VALUES[@]} * \
    ${#DATASET_TYPE_VALUES[@]} * \
    ${#PARTITION_THREAD_VALUES[@]} * \
    ${#JOIN_THREAD_VALUES[@]} * \
    ${#PARTITION_SCHEDULE_VALUES[@]} * \
    ${#JOIN_SCHEDULE_VALUES[@]} * \
    ${#PARTITION_CHUNK_VALUES[@]} * \
    ${#JOIN_CHUNK_VALUES[@]} * \
    ${#PARTITION_BLOCK_SIZE_VALUES[@]} * \
    ${#MPI_PARTITION_STRATEGY_VALUES[@]} * \
    REPEAT_COUNT \
))

COUNT=0

echo
echo "Benchmark target:            $EXECUTABLE_TARGET"
echo "Compiled executable:         $MAKE_TARGET"
echo "Grid source:                 $GRID_CONFIG"
echo "Node values:                 ${NODE_VALUES[*]}"
echo "MPI process values:          ${MPI_PROCESS_VALUES[*]}"
echo "Valid MPI layouts:           $VALID_MPI_LAYOUTS"
echo "N values:                    ${N_VALUES[*]}"
echo "P values:                    ${P_VALUES[*]}"
echo "Seeds:                       ${SEED_VALUES[*]}"
echo "Max-key values:              ${MAX_KEY_VALUES[*]}"
echo "Dataset-type values:         ${DATASET_TYPE_VALUES[*]}"
echo "Partition thread values:     ${PARTITION_THREAD_VALUES[*]}"
echo "Join thread values:          ${JOIN_THREAD_VALUES[*]}"
echo "Partition schedule values:   ${PARTITION_SCHEDULE_VALUES[*]}"
echo "Join schedule values:        ${JOIN_SCHEDULE_VALUES[*]}"
echo "Partition chunk values:      ${PARTITION_CHUNK_VALUES[*]}"
echo "Join chunk values:           ${JOIN_CHUNK_VALUES[*]}"
echo "Partition block-size values: ${PARTITION_BLOCK_SIZE_VALUES[*]}"
echo "MPI partition strategies:    ${MPI_PARTITION_STRATEGY_VALUES[*]}"
echo "Repeat count:                $REPEAT_COUNT"
echo "Total runs:                  $TOTAL"
echo

for ((RUN_INDEX=1; RUN_INDEX<=REPEAT_COUNT; RUN_INDEX++)); do
    for NODE_COUNT in "${NODE_VALUES[@]}"; do
        for MPI_PROCESS_COUNT in "${MPI_PROCESS_VALUES[@]}"; do
            if ! valid_mpi_layout "$NODE_COUNT" "$MPI_PROCESS_COUNT"; then
                continue
            fi
            TASKS_PER_NODE=$((MPI_PROCESS_COUNT / NODE_COUNT))
            for N in "${N_VALUES[@]}"; do
                for P in "${P_VALUES[@]}"; do
                    for SEED in "${SEED_VALUES[@]}"; do
                        for MAX_KEY in "${MAX_KEY_VALUES[@]}"; do
                            for DATASET_TYPE in "${DATASET_TYPE_VALUES[@]}"; do
                                for PARTITION_THREADS in "${PARTITION_THREAD_VALUES[@]}"; do
                                    for JOIN_THREADS in "${JOIN_THREAD_VALUES[@]}"; do
                                        for PARTITION_SCHEDULE in "${PARTITION_SCHEDULE_VALUES[@]}"; do
                                            for JOIN_SCHEDULE in "${JOIN_SCHEDULE_VALUES[@]}"; do
                                                for PARTITION_CHUNK in "${PARTITION_CHUNK_VALUES[@]}"; do
                                                    for JOIN_CHUNK in "${JOIN_CHUNK_VALUES[@]}"; do
                                                        for PARTITION_BLOCK_SIZE in "${PARTITION_BLOCK_SIZE_VALUES[@]}"; do
                                                            for MPI_PARTITION_STRATEGY in "${MPI_PARTITION_STRATEGY_VALUES[@]}"; do

                                                                COUNT=$((COUNT + 1))

                                                                printf "[%d/%d] run=%d/%d nodes=%s mpi_processes=%s tasks_per_node=%s N=%s P=%s seed=%s max_key=%s dataset_type=%s p_threads=%s j_threads=%s p_sched=%s j_sched=%s p_chunk=%s j_chunk=%s p_block=%s mpi_strategy=%s --> " \
                                                                    "$COUNT" "$TOTAL" "$RUN_INDEX" "$REPEAT_COUNT" "$NODE_COUNT" "$MPI_PROCESS_COUNT" "$TASKS_PER_NODE" "$N" "$P" "$SEED" "$MAX_KEY" "$DATASET_TYPE" \
                                                                    "$PARTITION_THREADS" "$JOIN_THREADS" "$PARTITION_SCHEDULE" "$JOIN_SCHEDULE" \
                                                                    "$PARTITION_CHUNK" "$JOIN_CHUNK" "$PARTITION_BLOCK_SIZE" "$MPI_PARTITION_STRATEGY"

                                                                runner_args=(
                                                                    "$RUNNER_SCRIPT"
                                                                    "$EXECUTABLE"
                                                                    "$NODE_COUNT"
                                                                    "$MPI_PROCESS_COUNT"
                                                                    "$N"
                                                                    "$N"
                                                                    "$SEED"
                                                                    "$MAX_KEY"
                                                                    "$P"
                                                                    "$DATASET_TYPE"
                                                                    "$PARTITION_THREADS"
                                                                    "$JOIN_THREADS"
                                                                    "$PARTITION_SCHEDULE"
                                                                    "$JOIN_SCHEDULE"
                                                                    "$PARTITION_CHUNK"
                                                                    "$JOIN_CHUNK"
                                                                    "$PARTITION_BLOCK_SIZE"
                                                                    "$MPI_PARTITION_STRATEGY"
                                                                )

                                                                sbatch --parsable --wait --nodes="$NODE_COUNT" --ntasks="$MPI_PROCESS_COUNT" --ntasks-per-node="$TASKS_PER_NODE" "${runner_args[@]}"

                                                            done
                                                        done
                                                    done
                                                done
                                            done
                                        done
                                    done
                                done
                            done
                        done
                    done
                done
            done
        done
    done
done

echo

make -B checker
./checker

make clean
