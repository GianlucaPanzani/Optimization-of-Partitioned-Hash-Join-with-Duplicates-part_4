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

is_nonnegative_int() {
    [[ "$1" =~ ^[0-9]+$ ]]
}

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
    NODE_COUNT=""
    MPI_PROCESS_COUNT=""
    NR=""
    NS=""
    SEED=""
    MAX_KEY=""
    P=""
    DATASET_TYPE=""
    PARTITION_THREADS=""
    JOIN_THREADS=""
    PARTITION_SCHEDULE=""
    JOIN_SCHEDULE=""
    PARTITION_CHUNK=""
    JOIN_CHUNK=""
    PARTITION_BLOCK_SIZE=""
    MPI_PARTITION_STRATEGY=""
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
            NODE_COUNT) NODE_COUNT="$value" ;;
            MPI_PROCESS_COUNT) MPI_PROCESS_COUNT="$value" ;;
            NR) NR="$value" ;;
            NS) NS="$value" ;;
            SEED) SEED="$value" ;;
            MAX_KEY) MAX_KEY="$value" ;;
            P) P="$value" ;;
            DATASET_TYPE) DATASET_TYPE="$value" ;;
            PARTITION_THREADS) PARTITION_THREADS="$value" ;;
            JOIN_THREADS) JOIN_THREADS="$value" ;;
            PARTITION_SCHEDULE) PARTITION_SCHEDULE="$value" ;;
            JOIN_SCHEDULE) JOIN_SCHEDULE="$value" ;;
            PARTITION_CHUNK) PARTITION_CHUNK="$value" ;;
            JOIN_CHUNK) JOIN_CHUNK="$value" ;;
            PARTITION_BLOCK_SIZE) PARTITION_BLOCK_SIZE="$value" ;;
            MPI_PARTITION_STRATEGY) MPI_PARTITION_STRATEGY="$value" ;;
            OUTPUT_CSV) OUTPUT_CSV="$value" ;;
            *)
                echo "Unknown case key '$key' in: $case_entry"
                return 1
                ;;
        esac
    done
}

require_case_values() {
    local missing=()
    local name
    for name in \
        NODE_COUNT MPI_PROCESS_COUNT NR NS SEED MAX_KEY P DATASET_TYPE \
        PARTITION_THREADS JOIN_THREADS PARTITION_SCHEDULE JOIN_SCHEDULE \
        PARTITION_CHUNK JOIN_CHUNK PARTITION_BLOCK_SIZE MPI_PARTITION_STRATEGY; do
        if [ -z "${!name}" ]; then
            missing+=("$name")
        fi
    done

    if [ "${#missing[@]}" -gt 0 ]; then
        echo "Scaling case is missing required keys: ${missing[*]}"
        return 1
    fi

    for name in NODE_COUNT MPI_PROCESS_COUNT NR NS SEED MAX_KEY P PARTITION_THREADS JOIN_THREADS PARTITION_BLOCK_SIZE; do
        if ! is_positive_int "${!name}"; then
            echo "$name must be a positive integer, received: ${!name}"
            return 1
        fi
    done

    for name in PARTITION_CHUNK JOIN_CHUNK; do
        if ! is_nonnegative_int "${!name}"; then
            echo "$name must be a non-negative integer, received: ${!name}"
            return 1
        fi
    done

    if ! valid_mpi_layout "$NODE_COUNT" "$MPI_PROCESS_COUNT"; then
        echo "Invalid MPI layout: NODE_COUNT=$NODE_COUNT MPI_PROCESS_COUNT=$MPI_PROCESS_COUNT"
        return 1
    fi
}

grid_name="$(basename "$GRID_CONFIG")"
grid_stem="${grid_name%.sh}"
default_output_csv="results/${EXECUTABLE_TARGET}_${grid_stem}.csv"
rm -f "$default_output_csv"

total=$(( ${#SCALING_CASES[@]} * REPEAT_COUNT ))
count=0

echo
echo "Scaling benchmark executable: $EXECUTABLE_TARGET"
echo "Grid source:                  $GRID_CONFIG"
echo "Cases:                        ${#SCALING_CASES[@]}"
echo "Repeat count:                 $REPEAT_COUNT"
echo "Default output CSV:           $default_output_csv"
echo "Total srun calls:             $total"
echo

for ((run_index=1; run_index<=REPEAT_COUNT; run_index++)); do
    for case_index in "${!SCALING_CASES[@]}"; do
        parse_case "${SCALING_CASES[$case_index]}"
        require_case_values

        if [ -z "$OUTPUT_CSV" ]; then
            OUTPUT_CSV="$default_output_csv"
        fi
        if [[ "$OUTPUT_CSV" != *.csv ]]; then
            echo "OUTPUT_CSV must end with .csv, received: $OUTPUT_CSV"
            exit 1
        fi

        tasks_per_node=$((MPI_PROCESS_COUNT / NODE_COUNT))
        max_threads="$PARTITION_THREADS"
        if [ "$JOIN_THREADS" -gt "$max_threads" ]; then
            max_threads="$JOIN_THREADS"
        fi

        export OMP_NUM_THREADS="$max_threads"
        export OMP_DISPLAY_ENV="${OMP_DISPLAY_ENV:-false}"

        count=$((count + 1))
        printf "[%d/%d] run=%d/%d case=%d/%d target=%s nodes=%s mpi_processes=%s N=(%s,%s) P=%s dataset_type=%s p_threads=%s j_threads=%s output=%s --> " \
            "$count" "$total" "$run_index" "$REPEAT_COUNT" "$((case_index + 1))" "${#SCALING_CASES[@]}" "$EXECUTABLE_TARGET" \
            "$NODE_COUNT" "$MPI_PROCESS_COUNT" "$NR" "$NS" "$P" "$DATASET_TYPE" "$PARTITION_THREADS" "$JOIN_THREADS" "$OUTPUT_CSV"

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
            --output-csv "$OUTPUT_CSV"
        )

        srun --mpi=pmix --nodes="$NODE_COUNT" --ntasks="$MPI_PROCESS_COUNT" --ntasks-per-node="$tasks_per_node" "${runner_args[@]}"
    done
done

echo
echo "Scaling benchmark completed. Results appended to $default_output_csv unless overridden per case."
