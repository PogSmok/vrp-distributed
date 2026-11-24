# vrp-bench

Runs vrp-core on files and reports how long each phase takes (ADR 0023). It
is built by the `cuda` and `cuda-release` presets; see `core/README.md`.
Take timings from `cuda-release`: the GPU kernels are optimised in both, but
the host code, such as reading the graph and snapping, only in Release.

```sh
vrp-bench distances --graph <directory> --problem <problem.json> [--max-snap-m <metres>] [--check]
vrp-bench solve --cvrplib <instance.vrp> [--vehicles <count>] [--seconds <s> | --steps <count>] [--seed <seed>]
vrp-bench solve --graph <directory> --problem <problem.json> [--max-snap-m <metres>] [--seconds <s> | --steps <count>] [--seed <seed>] [--result <result.json>]
```

Each takes `--device gpu` (the default) or `--device cpu`, the baseline the
GPU's speed is measured against.

`--graph` is a graph version's directory, as the preprocessor writes it:
its `manifest.json` names `graph.bin` and, for a simplified graph,
`edge_geometry.bin`.

`distances` snaps the problem's stops to the graph (500 m at most by
default), computes their distance matrix on the GPU, and with `--check`
compares it with the CPU reference, which is slow on large graphs.

`solve` runs one solver island (ADR 0018) for 10 seconds by default,
printing each better solution, then checks the best against the matrix. A
CVRPLIB instance (EUC_2D, or EXPLICIT FULL_MATRIX) does not fix the fleet,
so by default it gets HGS-CVRP's: ⌈1.3 × total demand ÷ capacity⌉ + 3
vehicles.

A road problem first gets its matrix as in `distances`, and ends with
FINALIZE (ADR 0025): every leg is routed on all CPU threads, the legs must
add up to the solver's cost, and `--result` writes the result artifact
(`docs/formats/result.md`).

Inputs come from the preprocessor (see `preprocessor/README.md`), from an
OpenStreetMap extract or synthetic:

```sh
vrp-preprocessor synthetic grid --width 2000 --height 2000 --output grid
vrp-preprocessor problem --graph grid --customers 5000 --output problem.json
vrp-bench distances --graph grid --problem problem.json
vrp-bench solve --graph grid --problem problem.json --result result.json
```

## GPU against CPU

`--device cpu` runs, on every CPU thread, what the GPU would: the distances
with Dijkstra from each stop (vrp-core's reference), and the solver's local
search with `reference::LocalSearch`, which shares the GPU's move code. With
the same `--seed`, both search alike step by step and end at the same best
cost, so a run with `--steps` times the same work on each:

```sh
vrp-bench solve --graph mazowieckie --problem problem.json --steps 300 --device gpu
vrp-bench solve --graph mazowieckie --problem problem.json --steps 300 --device cpu
```

Each prints its distance time, its steps per second and its best cost. A
step's CPU work, such as Split and applying moves, is the same on both, so
the solver's speedup is that of the local search diluted by it. Time a
`cuda-release` build: in Debug, the CPU path is not optimised.

The problem file format is `docs/formats/problem.md`. Parsing it is the
`vrp_bench_files` library, which builds without CUDA so that its tests run
in every CI job.
