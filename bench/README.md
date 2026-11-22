# vrp-bench

Runs vrp-core on files and reports how long each phase takes (ADR 0023). It
is built by the `cuda` and `cuda-release` presets; see `core/README.md`.
Take timings from `cuda-release`: the GPU kernels are optimised in both, but
the host code, such as reading the graph and snapping, only in Release.

```sh
vrp-bench distances --graph <graph.bin> --problem <problem.json> [--max-snap-m <metres>] [--check]
```

`distances` snaps the problem's stops to the graph (500 m at most by
default), computes their distance matrix on the GPU, and with `--check`
compares it with the CPU reference, which is slow on large graphs.

Inputs come from the preprocessor:

```sh
vrp-preprocessor synthetic grid --width 2000 --height 2000 --output grid
vrp-preprocessor problem --graph grid --customers 5000 --output problem.json
vrp-bench distances --graph grid/graph.bin --problem problem.json
```

The problem file format is `docs/formats/problem.md`. Parsing it is the
`vrp_bench_files` library, which builds without CUDA so that its tests run
in every CI job.
