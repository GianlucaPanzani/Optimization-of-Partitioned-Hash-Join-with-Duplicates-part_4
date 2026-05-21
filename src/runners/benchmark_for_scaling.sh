#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXE_DIR="$SCRIPT_DIR/.."

cd "$EXE_DIR"
mkdir -p out err results compilation

if [ "$#" -lt 1 ] || [ "$#" -gt 3 ]; then
    echo "Usage: $0 EXECUTABLE_NAME [GRID_CONFIG(optional)] [REPEAT_COUNT(optional)]"
    echo "  EXECUTABLE_NAME: make target to compile and benchmark, without .cpp"
    echo "  GRID_CONFIG: shell file defining SCALING_CASES"
    echo "  REPEAT_COUNT: positive integer (default: 1)"
    exit 1
fi

EXECUTABLE_INPUT="$1"
EXECUTABLE_TARGET="$(basename "$EXECUTABLE_INPUT")"
EXECUTABLE_TARGET="${EXECUTABLE_TARGET%.cpp}"
GRID_CONFIG="$EXE_DIR/grid/strong_scaling_grid_search.sh"
REPEAT_COUNT="1"

if [ "$#" -eq 2 ]; then
    if [[ "$2" =~ ^[0-9]+$ ]]; then
        REPEAT_COUNT="$2"
    else
        if [[ "$2" = /* ]]; then
            GRID_CONFIG="$2"
        else
            GRID_CONFIG="$EXE_DIR/$2"
        fi
    fi
elif [ "$#" -eq 3 ]; then
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

source "$GRID_CONFIG"

grid_name="$(basename "$GRID_CONFIG")"
grid_stem="${grid_name%.sh}"
grid_stem="${grid_stem%_grid_search}"
if [[ "$grid_stem" == *weak* ]]; then
    SCALING_KIND="weak"
elif [[ "$grid_stem" == *strong* ]]; then
    SCALING_KIND="strong"
else
    echo "Cannot infer scaling kind from grid path: $GRID_CONFIG"
    echo "Use a grid filename containing 'weak' or 'strong'."
    exit 1
fi

# Shared/fixed scaling parameters. Keep the grids focused only on the values
# that differ between strong and weak scaling cases.
DEFAULT_NODE_COUNT=1
DEFAULT_MPI_PROCESS_COUNT=1
SEED=13
MAX_KEY=1000000
P=256
PARTITION_THREADS=16
JOIN_THREADS=16
PARTITION_SCHEDULE=guided
JOIN_SCHEDULE=guided
PARTITION_CHUNK=8
JOIN_CHUNK=8
PARTITION_BLOCK_SIZE=32768
MPI_PARTITION_STRATEGY=block

STRONG_NR=50000000
STRONG_NS=50000000

DATASET_TYPE_VALUES=(
    uniform
    skewed_90_5
    skewed_90_1
)

if ! declare -p SCALING_CASES >/dev/null 2>&1; then
    echo "Grid configuration must define SCALING_CASES as a non-empty array"
    exit 1
fi

if [ "${#SCALING_CASES[@]}" -eq 0 ]; then
    echo "SCALING_CASES is empty in grid configuration: $GRID_CONFIG"
    exit 1
fi

if ! make -B "$EXECUTABLE_TARGET"; then
    echo "Compilation failed or unknown make target: $EXECUTABLE_TARGET"
    exit 1
fi

EXECUTABLE="$EXE_DIR/$EXECUTABLE_TARGET"
if [ ! -x "$EXECUTABLE" ]; then
    echo "Compiled executable not found or not executable: $EXECUTABLE"
    exit 1
fi

is_positive_int() {
    [[ "$1" =~ ^[0-9]+$ ]] && [ "$1" -gt 0 ]
}

valid_mpi_layout() {
    local nodes="$1"
    local processes="$2"
    is_positive_int "$nodes" || return 1
    is_positive_int "$processes" || return 1
    [ "$processes" -ge "$nodes" ] || return 1
    [ $((processes % nodes)) -eq 0 ]
}

reset_case_values() {
    NR=""
    NS=""
    CASE_NODE_COUNT=""
    CASE_MPI_PROCESS_COUNT=""
    OUTPUT_CSV=""
}

parse_case() {
    local case_entry="$1"
    local pair key value

    reset_case_values
    for pair in $case_entry; do
        if [[ "$pair" != *=* ]]; then
            echo "Invalid case token '$pair'. Expected KEY=VALUE in: $case_entry"
            return 1
        fi
        key="${pair%%=*}"
        value="${pair#*=}"
        case "$key" in
            NR) NR="$value" ;;
            NS) NS="$value" ;;
            MPI_NODES) CASE_NODE_COUNT="$value" ;;
            MPI_PROCESSES|MPI_PROCESS_COUNT) CASE_MPI_PROCESS_COUNT="$value" ;;
            OUTPUT_CSV) OUTPUT_CSV="$value" ;;
            *)
                echo "Unknown or shared case key '$key' in: $case_entry"
                echo "Only NR, NS, MPI_NODES, MPI_PROCESSES, and OUTPUT_CSV are accepted in scaling grids."
                return 1
                ;;
        esac
    done
}

require_case_values() {
    local missing=()
    local name
    if [ "$SCALING_KIND" = "weak" ]; then
        for name in NR NS; do
            if [ -z "${!name}" ]; then
                missing+=("$name")
            fi
        done
    fi

    if [ "${#missing[@]}" -gt 0 ]; then
        echo "Scaling case is missing required keys: ${missing[*]}"
        return 1
    fi

    if [ "$SCALING_KIND" = "strong" ]; then
        NR="$STRONG_NR"
        NS="$STRONG_NS"
    fi

    for name in NR NS PARTITION_THREADS JOIN_THREADS; do
        if ! is_positive_int "${!name}"; then
            echo "$name must be a positive integer, received: ${!name}"
            return 1
        fi
    done

    if [ -n "$CASE_NODE_COUNT" ] && ! is_positive_int "$CASE_NODE_COUNT"; then
        echo "MPI_NODES must be a positive integer, received: $CASE_NODE_COUNT"
        return 1
    fi

    if [ -n "$CASE_MPI_PROCESS_COUNT" ] && ! is_positive_int "$CASE_MPI_PROCESS_COUNT"; then
        echo "MPI_PROCESSES must be a positive integer, received: $CASE_MPI_PROCESS_COUNT"
        return 1
    fi

    NODE_COUNT="${CASE_NODE_COUNT:-$DEFAULT_NODE_COUNT}"
    if [ -n "$CASE_MPI_PROCESS_COUNT" ]; then
        MPI_PROCESS_COUNT="$CASE_MPI_PROCESS_COUNT"
    elif [ -n "$CASE_NODE_COUNT" ]; then
        MPI_PROCESS_COUNT="$CASE_NODE_COUNT"
    else
        MPI_PROCESS_COUNT="$DEFAULT_MPI_PROCESS_COUNT"
    fi

    if ! valid_mpi_layout "$NODE_COUNT" "$MPI_PROCESS_COUNT"; then
        echo "Invalid MPI layout: MPI_NODES=$NODE_COUNT MPI_PROCESSES=$MPI_PROCESS_COUNT"
        return 1
    fi
}

default_output_csv="results/${grid_stem}.csv"
rm -f "$default_output_csv"

total=$(( ${#SCALING_CASES[@]} * ${#DATASET_TYPE_VALUES[@]} * REPEAT_COUNT ))
count=0

echo
echo "Scaling benchmark executable: $EXECUTABLE_TARGET"
echo "Grid source:                  $GRID_CONFIG"
echo "Scaling kind:                 $SCALING_KIND"
echo "Cases:                        ${#SCALING_CASES[@]}"
echo "Dataset-type values:          ${DATASET_TYPE_VALUES[*]}"
echo "Fixed P/seed/max-key:         P=$P seed=$SEED max_key=$MAX_KEY"
echo "Fixed OpenMP threads:         partition=$PARTITION_THREADS join=$JOIN_THREADS"
echo "Fixed schedules/chunks:       partition=$PARTITION_SCHEDULE/$PARTITION_CHUNK join=$JOIN_SCHEDULE/$JOIN_CHUNK block_size=$PARTITION_BLOCK_SIZE"
echo "Default MPI layout:           nodes=$DEFAULT_NODE_COUNT mpi_processes=$DEFAULT_MPI_PROCESS_COUNT strategy=$MPI_PARTITION_STRATEGY"
echo "Repeat count:                 $REPEAT_COUNT"
echo "Default output CSV:           $default_output_csv"
echo "Total srun calls:             $total"
echo

for ((run_index=1; run_index<=REPEAT_COUNT; run_index++)); do
    for case_index in "${!SCALING_CASES[@]}"; do
        parse_case "${SCALING_CASES[$case_index]}"
        require_case_values

        for DATASET_TYPE in "${DATASET_TYPE_VALUES[@]}"; do
            current_output_csv="$OUTPUT_CSV"
            if [ -z "$current_output_csv" ]; then
                current_output_csv="$default_output_csv"
            fi
            if [[ "$current_output_csv" != *.csv ]]; then
                echo "OUTPUT_CSV must end with .csv, received: $current_output_csv"
                exit 1
            fi

            tasks_per_node=$((MPI_PROCESS_COUNT / NODE_COUNT))
            export OMP_NUM_THREADS="$PARTITION_THREADS"
            export OMP_DISPLAY_ENV="${OMP_DISPLAY_ENV:-false}"

            count=$((count + 1))
            printf "[%d/%d] run=%d/%d case=%d/%d target=%s nodes=%s mpi_processes=%s N=(%s,%s) P=%s dataset_type=%s p_threads=%s j_threads=%s output=%s --> " \
                "$count" "$total" "$run_index" "$REPEAT_COUNT" "$((case_index + 1))" "${#SCALING_CASES[@]}" "$EXECUTABLE_TARGET" \
                "$NODE_COUNT" "$MPI_PROCESS_COUNT" "$NR" "$NS" "$P" "$DATASET_TYPE" "$PARTITION_THREADS" "$JOIN_THREADS" "$current_output_csv"

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
                --output-csv "$current_output_csv"
            )

            srun --mpi=pmix --nodes="$NODE_COUNT" --ntasks="$MPI_PROCESS_COUNT" --ntasks-per-node="$tasks_per_node" "${runner_args[@]}"
        done
    done
done

echo
make clean
