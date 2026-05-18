#!/bin/bash
#SBATCH --job-name=strong_scaling
#SBATCH --time=00:15:00
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=32
#SBATCH --partition=normal
#SBATCH --nodelist=node07
#SBATCH --output=out/strong_scaling-%j.log
#SBATCH --error=err/strong_scaling-%j.log

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXE_DIR="$SCRIPT_DIR/.."

cd "$EXE_DIR"
mkdir -p out err results compilation

make -B strong_scaling

export OMP_NUM_THREADS=64
export OMP_DISPLAY_ENV="${OMP_DISPLAY_ENV:-false}"

rm results/strong_scaling.csv

srun ./strong_scaling

rm strong_scaling