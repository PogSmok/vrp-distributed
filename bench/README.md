# vrp-bench

Runs vrp-core on files and reports how long each phase takes (ADR 0023). It
is built by the `cuda` and `cuda-release` presets; see `core/README.md`.
Take timings from `cuda-release`: the GPU kernels are optimised in both, but
the host code, such as reading the graph and snapping, only in Release.

```sh
vrp-bench distances --graph <directory> --problem <problem.json> [--max-snap-m <metres>] [--check]
vrp-bench solve --cvrplib <instance.vrp> [--vehicles <count>] [--seconds <s>] [--seed <seed>]
vrp-bench solve --graph <directory> --problem <problem.json> [--max-snap-m <metres>] [--seconds <s>] [--seed <seed>] [--result <result.json>]
```

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

Inputs come from the preprocessor:

```sh
vrp-preprocessor synthetic grid --width 2000 --height 2000 --output grid
vrp-preprocessor problem --graph grid --customers 5000 --output problem.json
vrp-bench distances --graph grid --problem problem.json
vrp-bench solve --graph grid --problem problem.json --result result.json
```

The problem file format is `docs/formats/problem.md`. Parsing it is the
`vrp_bench_files` library, which builds without CUDA so that its tests run
in every CI job.
