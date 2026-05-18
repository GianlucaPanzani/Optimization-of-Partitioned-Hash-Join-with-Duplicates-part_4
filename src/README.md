# Optimization of Partitioned Hash Join with Duplicates - Module 3

This repository contains a sequential partitioned hash join baseline, several
OpenMP implementations, and Slurm runners used to benchmark them.

## Layout

- `src/*.cpp`: C++ implementations and scaling benchmarks
- `src/Makefile`: build targets for all executables
- `src/grid/*.sh`: benchmark grid definitions
- `src/runners/*.sh`: Slurm-based benchmark runners
- `src/results/*.csv`: generated benchmark results
- `src/out/`, `src/err/`: Slurm output and error logs

## Build

Build from `src/`:

```bash
cd src
make <target>
```

Available targets:

```bash
make hashjoin_seq
make hashjoin_omp_loop
make hashjoin_omp_loop_wb
make hashjoin_omp_task
make hashjoin_omp_task_wb
make hashjoin_omp_taskloop
make hashjoin_omp_taskloop_wb
make strong_scaling
make weak_scaling
make checker
```

Use `make all` to build every executable.

## Run a Single Executable

Each hash join `.cpp` file is built through the matching Makefile target. After
building, run the executable from `src/`.

Minimal sequential example:

```bash
./hashjoin_seq -nr 1000000 -ns 1000000 -seed 13 -max-key 1000000 -p 256 --dataset-type uniform
```

OpenMP loop example:

```bash
export OMP_NUM_THREADS=32
./hashjoin_omp_loop \
  -nr 1000000 -ns 1000000 -seed 13 -max-key 1000000 -p 256 \
  --dataset-type uniform \
  --partition-threads 32 --join-threads 32 \
  --partition-schedule guided --join-schedule guided \
  --partition-chunk 8 --join-chunk 8 \
  --partition-block-size 32768
```

OpenMP task example:

```bash
export OMP_NUM_THREADS=32
./hashjoin_omp_task \
  -nr 1000000 -ns 1000000 -seed 13 -max-key 1000000 -p 256 \
  --dataset-type uniform \
  --partition-threads 32 --join-threads 32 \
  --partition-block-size 32768 \
  --partition-task-blocks 2 \
  --join-task-partitions 4 \
  --offset-task-partitions 2
```

OpenMP taskloop example:

```bash
export OMP_NUM_THREADS=32
./hashjoin_omp_taskloop \
  -nr 1000000 -ns 1000000 -seed 13 -max-key 1000000 -p 256 \
  --dataset-type uniform \
  --partition-threads 32 --join-threads 32 \
  --partition-block-size 32768 \
  --partition-task-grain 2 \
  --join-task-grain 4 \
  --offset-task-grain 2
```

The `_wb` executables use the same command-line options as their non-`_wb`
counterparts. Each run prints the checksums and appends one row to
`results/<executable>.csv`.

Supported dataset types are `uniform` and skewed distributions such as
`skewed_90_5` or `skewed_90_1`.

## Run the Benchmark Runners

The generic benchmark launcher compiles the selected target, expands the grid,
submits one Slurm job per configuration through `run_with_slurm.sh`, and runs
the checker at the end.

Run it from `src/runners/`:

```bash
./benchmark.sh <target> [grid/<grid_file>.sh] [repeat_count]
```

Examples:

```bash
cd src/runners
./benchmark.sh hashjoin_seq grid/seq_grid_search.sh 5
./benchmark.sh hashjoin_omp_loop grid/omp_loop_grid_search.sh 5
./benchmark.sh hashjoin_omp_loop_wb grid/omp_loop_grid_search.sh 5
./benchmark.sh hashjoin_omp_task grid/omp_task_grid_search.sh 5
./benchmark.sh hashjoin_omp_task_wb grid/omp_task_grid_search.sh 5
./benchmark.sh hashjoin_omp_taskloop grid/omp_taskloop_grid_search.sh 5
./benchmark.sh hashjoin_omp_taskloop_wb grid/omp_taskloop_grid_search.sh 5
```

Scaling jobs are separate Slurm scripts with hardcoded datasets and thread
counts:

```bash
sbatch run_strong_scaling.sh
sbatch run_weak_scaling.sh
```

To launch the whole benchmark suite, run `run_all.sh` from the runners folder:

```bash
./run_all.sh
```

## Cleaning

Useful cleanup commands from `src/`:

```bash
make clean
make cleanlogs
make cleanall
make cleanall_seq
make cleanall_omp_loop
make cleanall_omp_loop_wb
make cleanall_omp_task
make cleanall_omp_task_wb
make cleanall_omp_taskloop
make cleanall_omp_taskloop_wb
make cleanall_strong_scaling
make cleanall_weak_scaling
```
