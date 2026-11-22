# vrp-core

The algorithms of the VRP solver as a standalone library (ADR 0023): no
networking, no storage, and no threads of its own. The Worker and the
benchmark CLI build on it.

## Contents

| Header                | What it is                                                                              | ADR        |
|-----------------------|-----------------------------------------------------------------------------------------|------------|
| `status.hpp`          | `Status` and `StatusOr`; the library never throws                                       | —          |
| `types.hpp`           | Integer costs, node IDs, edge cost choice                                               | 0020       |
| `graph.hpp`           | Immutable road graph in compressed sparse rows, both directions                         | 0005       |
| `graph_file.hpp`      | Parses `graph.bin` (`docs/formats/graph.md`) into a `RoadGraph`, with the reader checks | 0027       |
| `distance_matrix.hpp` | `(n+1) × (n+1)` asymmetric cost matrix, depot first                                     | 0011       |
| `solution.hpp`        | Problem and solution checks, route cost evaluation                                      | 0006, 0025 |
| `leg_router.hpp`      | Bidirectional Dijkstra for route geometry; batched over a caller-supplied `ParallelFor` | 0025       |
| `geo.hpp`             | Great-circle distance and position checks                                               | 0006       |
| `snapping.hpp`        | Snaps stops to their nearest nodes through a grid index; matches the reference exactly  | 0006       |
| `reference/`          | CPU oracles: Dijkstra, distance matrix, brute-force snapping. Never used in production  | 0023       |
| `gpu/`                | `DeviceGraph`, `DistanceBatch` and `ComputeDistanceRows`: the distance phase on the GPU | 0011, 0017 |

The GPU code is a separate target, `vrp_core_gpu`, built only with
`VRP_ENABLE_CUDA`, so `vrp_core` builds and passes its tests without CUDA.
The GPU tests are labelled `gpu` and checked against `reference/`. The solver
kernels (0018) come next.

## Conventions

- Google C++ style, enforced by `.clang-format` and `.clang-tidy` at the
  repository root; CI fails on any finding.
- No exceptions: fallible functions return `Status` or `StatusOr<T>`, and
  `VRP_RETURN_IF_ERROR` / `VRP_ASSIGN_OR_RETURN` (`status_macros.hpp`)
  propagate them.
- Running out of memory is fatal. Allocation failures throw `std::bad_alloc`
  from the standard library, and nothing catches it, so the worker exits and
  is restarted (ADR 0024).
- Costs are unsigned integers: `Cost` (32-bit) per edge or path, `RouteCost`
  (64-bit) for sums, so results are exact and reproducible (0020).

## Build and test

```sh
cmake --preset default # or: release, asan, tsan, coverage
cmake --build --preset default
ctest --preset default
```

The `cuda` preset adds the GPU code and its tests. It needs CUDA 12.8 or
a later 12.x.

```sh
cmake --preset cuda
cmake --build --preset cuda
ctest --preset cuda
```
