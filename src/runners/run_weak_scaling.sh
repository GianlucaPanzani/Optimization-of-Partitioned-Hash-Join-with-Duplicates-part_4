#!/bin/bash
#SBATCH --job-name=weak_scaling
#SBATCH --time=00:15:00
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=32
#SBATCH --partition=normal
#SBATCH --nodelist=node07
#SBATCH --output=out/weak_scaling-%j.log
#SBATCH --error=err/weak_scaling-%j.log

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXE_DIR="$SCRIPT_DIR/.."

cd "$EXE_DIR"
mkdir -p out err results compilation

make -B weak_scaling

export OMP_NUM_THREADS=32
export OMP_DISPLAY_ENV="${OMP_DISPLAY_ENV:-false}"

rm results/weak_scaling.csv

srun ./weak_scaling

rm weak_scaling
