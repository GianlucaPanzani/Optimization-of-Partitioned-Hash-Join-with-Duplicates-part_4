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
GRID_CONFIG="$EXE_DIR/grid/omp_loop_grid_search.sh"
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

REQUIRED_ARRAYS=(
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
)

case "$EXECUTABLE_TARGET" in
    hashjoin_omp_task|hashjoin_omp_task_wb)
        PARTITION_PARAM_ARRAY="PARTITION_TASK_BLOCKS_VALUES"
        JOIN_PARAM_ARRAY="JOIN_TASK_PARTITIONS_VALUES"
        OFFSET_PARAM_ARRAY="OFFSET_TASK_PARTITIONS_VALUES"
        PARTITION_PARAM_LABEL="Partition task-block values"
        JOIN_PARAM_LABEL="Join task-partition values"
        OFFSET_PARAM_LABEL="Offset task-partition values"
        PROGRESS_PARAM_LABELS="p_task_blocks=%s j_task_partitions=%s offset_task_partitions=%s"
        ;;
    hashjoin_omp_taskloop|hashjoin_omp_taskloop_wb)
        PARTITION_PARAM_ARRAY="PARTITION_TASKLOOP_GRAIN_VALUES"
        JOIN_PARAM_ARRAY="JOIN_TASKLOOP_GRAIN_VALUES"
        OFFSET_PARAM_ARRAY="OFFSET_TASKLOOP_GRAIN_VALUES"
        PARTITION_PARAM_LABEL="Partition taskloop-grain values"
        JOIN_PARAM_LABEL="Join taskloop-grain values"
        OFFSET_PARAM_LABEL="Offset taskloop-grain values"
        PROGRESS_PARAM_LABELS="p_taskloop_grain=%s j_taskloop_grain=%s offset_taskloop_grain=%s"
        ;;
    *)
        PARTITION_PARAM_ARRAY="PARTITION_TASK_GRAIN_VALUES"
        JOIN_PARAM_ARRAY="JOIN_TASK_GRAIN_VALUES"
        OFFSET_PARAM_ARRAY="OFFSET_TASK_GRAIN_VALUES"
        PARTITION_PARAM_LABEL="Partition task-grain values"
        JOIN_PARAM_LABEL="Join task-grain values"
        OFFSET_PARAM_LABEL="Offset task-grain values"
        PROGRESS_PARAM_LABELS="p_task_grain=%s j_task_grain=%s offset_task_grain=%s"
        ;;
esac

REQUIRED_ARRAYS+=(
    "$PARTITION_PARAM_ARRAY"
    "$JOIN_PARAM_ARRAY"
    "$OFFSET_PARAM_ARRAY"
)

for ARRAY_NAME in "${REQUIRED_ARRAYS[@]}"; do
    if ! require_non_empty_array "$ARRAY_NAME"; then
        echo "Grid configuration must define non-empty array: $ARRAY_NAME"
        exit 1
    fi
done

eval "PARTITION_PARAM_VALUES=(\"\${${PARTITION_PARAM_ARRAY}[@]}\")"
eval "JOIN_PARAM_VALUES=(\"\${${JOIN_PARAM_ARRAY}[@]}\")"
eval "OFFSET_PARAM_VALUES=(\"\${${OFFSET_PARAM_ARRAY}[@]}\")"

case "$EXECUTABLE_TARGET" in
    hashjoin_seq)
        make cleanall_seq
        ;;
    hashjoin_omp_loop)
        make cleanall_omp_loop
        ;;
    hashjoin_omp_loop_wb)
        make cleanall_omp_loop_wb
        ;;
    hashjoin_omp_task)
        make cleanall_omp_task
        ;;
    hashjoin_omp_task_wb)
        make cleanall_omp_task_wb
        ;;
    hashjoin_omp_taskloop)
        make cleanall_omp_taskloop
        ;;
    hashjoin_omp_taskloop_wb)
        make cleanall_omp_taskloop_wb
        ;;
    *)
        echo "Unknown executable target: $EXECUTABLE_TARGET"
        echo "Add the target to the Makefile and to benchmark.sh if needed."
        exit 1
        ;;
esac

mkdir -p compilation

if ! make -B "$EXECUTABLE_TARGET"; then
    echo "Compilation failed or unknown make target: $EXECUTABLE_TARGET"
    exit 1
fi

EXECUTABLE="$EXE_DIR/$EXECUTABLE_TARGET"

if [ ! -x "$EXECUTABLE" ]; then
    echo "Compiled executable not found or not executable: $EXECUTABLE"
    exit 1
fi

TOTAL=$(( \
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
    ${#PARTITION_PARAM_VALUES[@]} * \
    ${#JOIN_PARAM_VALUES[@]} * \
    ${#OFFSET_PARAM_VALUES[@]} * \
    REPEAT_COUNT \
))

COUNT=0

echo
echo "Benchmark executable:        $EXECUTABLE_TARGET"
echo "Grid source:                 $GRID_CONFIG"
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
echo "$PARTITION_PARAM_LABEL: ${PARTITION_PARAM_VALUES[*]}"
echo "$JOIN_PARAM_LABEL:      ${JOIN_PARAM_VALUES[*]}"
echo "$OFFSET_PARAM_LABEL:    ${OFFSET_PARAM_VALUES[*]}"
echo "Repeat count:                $REPEAT_COUNT"
echo "Total runs:                  $TOTAL"
echo

for ((RUN_INDEX=1; RUN_INDEX<=REPEAT_COUNT; RUN_INDEX++)); do
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
                                                    for PARTITION_PARAM in "${PARTITION_PARAM_VALUES[@]}"; do
                                                        for JOIN_PARAM in "${JOIN_PARAM_VALUES[@]}"; do
                                                            for OFFSET_PARAM in "${OFFSET_PARAM_VALUES[@]}"; do

                                                            COUNT=$((COUNT + 1))

                                                            printf "[%d/%d] run=%d/%d N=%s P=%s seed=%s max_key=%s dataset_type=%s p_threads=%s j_threads=%s p_sched=%s j_sched=%s p_chunk=%s j_chunk=%s p_block=%s ${PROGRESS_PARAM_LABELS} --> " \
                                                                "$COUNT" "$TOTAL" "$RUN_INDEX" "$REPEAT_COUNT" "$N" "$P" "$SEED" "$MAX_KEY" "$DATASET_TYPE" \
                                                                "$PARTITION_THREADS" "$JOIN_THREADS" "$PARTITION_SCHEDULE" "$JOIN_SCHEDULE" \
                                                                "$PARTITION_CHUNK" "$JOIN_CHUNK" "$PARTITION_BLOCK_SIZE" "$PARTITION_PARAM" \
                                                                "$JOIN_PARAM" "$OFFSET_PARAM"

                                                            runner_args=(
                                                                "$RUNNER_SCRIPT"
                                                                "$EXECUTABLE"
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
                                                                "$PARTITION_PARAM"
                                                                "$JOIN_PARAM"
                                                                "$OFFSET_PARAM"
                                                            )

                                                            sbatch --parsable --wait "${runner_args[@]}"

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
