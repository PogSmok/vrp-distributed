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
| `edge_geometry.hpp`   | Each edge's road shape, parsed from `edge_geometry.bin` with the reader checks          | 0027       |
| `distance_matrix.hpp` | `(n+1) × (n+1)` asymmetric cost matrix, depot first                                     | 0011       |
| `solution.hpp`        | Problem and solution checks, route cost evaluation                                      | 0006, 0025 |
| `leg_router.hpp`      | Bidirectional Dijkstra for route geometry; batched over a caller-supplied `ParallelFor` | 0025       |
| `result.hpp`          | FINALIZE: checks a solution, routes its legs, matches the cost, builds the polylines    | 0025       |
| `result_file.hpp`     | Writes the result artifact (`docs/formats/result.md`)                                   | 0016, 0028 |
| `geo.hpp`             | Great-circle distance and position checks                                               | 0006       |
| `snapping.hpp`        | Snaps stops to their nearest nodes through a grid index; matches the reference exactly  | 0006       |
| `reference/`          | CPU code: Dijkstra, distance rows, brute-force snapping, the local search               | 0023, 0037 |
| `solver/`             | The `Island`: Split, order crossover, the HGS population, on any local search           | 0018       |
| `gpu/`                | The distance phase, the GPU local search, and `CreateIsland` with it                    | 0017, 0018 |

The GPU code is a separate target, `vrp_core_gpu`, built only with
`VRP_ENABLE_CUDA`, so `vrp_core` builds and passes its tests without CUDA.
The GPU tests are labelled `gpu` and checked against `reference/`.

The local search's moves (`src/solver/local_search_moves.hpp`) compile for
both the GPU and the CPU. `reference::LocalSearch` runs them on CPU threads
and returns exactly the GPU's solutions, so an `Island` made with
`CreateOnCpu` searches as one from `gpu::CreateIsland` does: the baseline
that vrp-bench's `--device cpu` times (`bench/README.md`), and the CPU
worker's search (ADR 0037). For that, each move's gain is rounded alike on
both, without the GPU's fused multiply-add.

## Solver

An `Island` (ADR 0018) follows HGS-CVRP, the hybrid genetic search of
Thibaut Vidal (see [References](#references)): giant tours, Split, order
crossover, a population ranked by cost and broken-pairs diversity, an
adaptive capacity penalty and repair. Its local search runs on the GPU for a
batch of children at once: every customer's best move with its nearest
neighbours is found in parallel, and the best moves that share no route are
applied. Its random draws come from `solver/random.hpp`, so the same seed
gives the same search on Windows and Linux. Differences from HGS-CVRP, each a
candidate for tuning:

- moves are relocate (one customer, or a pair in either orientation), swap,
  intra-route 2-opt and 2-opt*; SWAP* and the reversing 2-opt* are not used;
- the giant tour concatenates routes in their order, not by polar angle;
- there are no restarts, since the task's time limit ends the search;
- children are bred a batch at a time, from the population as it was before
  the batch, where HGS-CVRP adds each child before breeding the next;
- order crossover keeps the whole first parent when the slice's end falls
  just before its start, where HGS-CVRP's loop keeps none of it.

Known limits, to be measured before they are changed:

- Each round of the local search re-uploads the whole batch and applies the
  chosen moves on the CPU. At thousands of customers the transfers and the
  CPU work may outweigh the kernel, which matters for the 1-second steps of
  ADR 0024. Uploading only the changed routes, or applying moves on the GPU,
  are the next steps if a measurement on the slowest GPU shows it.
- The P islands on one GPU (ADR 0013) share one `DeviceMatrix` but step in
  turn. Batching the children of all islands together belongs in the worker's
  design.

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

## References

- T. Vidal, T. G. Crainic, M. Gendreau, N. Lahrichi and W. Rei (2012). A
  hybrid genetic algorithm for multidepot and periodic vehicle routing
  problems. *Operations Research* 60(3). The hybrid genetic search: the
  population, biased fitness and diversity management.
- T. Vidal (2016). Split algorithm in O(n) for the capacitated vehicle
  routing problem. *Computers & Operations Research* 69. The algorithm of
  `solver/split.cpp`.
- T. Vidal (2022). Hybrid genetic search for the CVRP: open-source
  implementation and SWAP\* neighborhood. *Computers & Operations Research*
  140. HGS-CVRP, which `solver/` follows and whose defaults it uses.
