# Optimization of Partitioned Hash Join with Duplicates - Module 4

This repository contains a sequential partitioned hash join baseline, OpenMP,
MPI, and hybrid MPI+OpenMP implementations, plus Slurm runners used to benchmark
them.

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
make hashjoin_omp
make hashjoin_mpi
make hashjoin_hybrid
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

OpenMP example:

```bash
export OMP_NUM_THREADS=32
./hashjoin_omp \
  -nr 1000000 -ns 1000000 -seed 13 -max-key 1000000 -p 256 \
  --dataset-type uniform \
  --partition-threads 32 --join-threads 32 \
  --partition-schedule guided --join-schedule guided \
  --partition-chunk 8 --join-chunk 8 \
  --partition-block-size 32768
```

MPI example:

```bash
srun --mpi=pmix --nodes=1 --ntasks=4 --ntasks-per-node=4 \
  ./hashjoin_mpi \
  -nr 1000000 -ns 1000000 -seed 13 -max-key 1000000 -p 256 \
  --dataset-type uniform \
  --mpi-nodes 1 --mpi-processes 4 \
  --mpi-partition-strategy cyclic
```

Hybrid MPI+OpenMP example:

```bash
export OMP_NUM_THREADS=16
srun --mpi=pmix --nodes=1 --ntasks=4 --ntasks-per-node=4 \
  ./hashjoin_hybrid \
  -nr 1000000 -ns 1000000 -seed 13 -max-key 1000000 -p 256 \
  --dataset-type uniform \
  --partition-threads 16 --join-threads 16 \
  --partition-schedule guided --join-schedule guided \
  --partition-chunk 4 --join-chunk 4 \
  --partition-block-size 32768 \
  --mpi-nodes 1 --mpi-processes 4 \
  --mpi-partition-strategy cyclic
```

Each run prints the checksums and appends one row to
`results/<executable>.csv`, unless `--output-csv` is provided.

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
./benchmark.sh hashjoin_omp grid/omp_grid_search.sh 5
./benchmark.sh hashjoin_mpi grid/mpi_grid_search.sh 5
./benchmark.sh hashjoin_hybrid_uniform grid/hybrid_uniform_grid_search.sh 5
./benchmark.sh hashjoin_hybrid_skew1 grid/hybrid_skew1_grid_search.sh 5
./benchmark.sh hashjoin_hybrid_skew2 grid/hybrid_skew2_grid_search.sh 5
```

Scaling jobs use `benchmark_for_scaling.sh`, which compiles the selected
executable and runs the cases defined in the scaling grid with `srun`:

```bash
cd src/runners
./benchmark_for_scaling.sh hashjoin_mpi grid/strong_scaling_grid_search.sh 5
./benchmark_for_scaling.sh hashjoin_mpi grid/weak_scaling_grid_search.sh 5
```

To launch the benchmark suite configured in `run_all.sh`, run it from the
runners folder:

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
make cleanall_omp
make cleanall_mpi
make cleanall_hybrid
```
