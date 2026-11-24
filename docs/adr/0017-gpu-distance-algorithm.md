# 0017. GPU distance algorithm: batched frontier Bellman-Ford on the plain graph

- **Status:** Accepted; batch size fixed by 0033
- **Date:** 2026-11-21
- **Related:** 0011, 0020, 0023, 0024

## Context

The distance phase needs exact costs between all stops on a graph of a few
million nodes, within 5 minutes for 5,000 customers on one GPU. A road
shortest path crosses thousands of edges, so one source cannot fill a GPU;
many must run together, in kernel launches well under a second (0024).

## Decision

A batched, frontier-based Bellman-Ford on the plain graph:
- `B` sources run at once. Distances are stored node-major, so one edge load
  serves all `B` sources.
- Distances are updated with the integer atomic minimum, written only when
  they improve.
- Several rounds run per kernel launch, and the "frontier empty" flag is
  read only every few launches. This keeps launch overhead low, which
  matters most under the Windows display driver model.
- `B` is set from free GPU memory: `B = 32` at 5 million nodes uses about
  640 MB.

If this misses the target, a new ADR switches to RPHAST.

## Alternatives considered

- **RPHAST on contraction hierarchies:** the fastest known method, but it
  needs a preprocessing step and a new graph format; it is the first
  fallback.
- **Contraction-hierarchy many-to-many on the CPU:** may beat the GPU, and is
  kept as an honest benchmark baseline.
- **A single-source GPU library (Gunrock):** one source cannot fill the GPU
  on road graphs, and its steps are hard to bound.
- **Multithreaded CPU Dijkstra:** about 10 minutes; kept as the correctness
  oracle (0023).

## Consequences

- Gain: exact results, with no preprocessing.
- Cost: more relaxations than Dijkstra; `B` and the rounds per launch must
  be tuned per GPU.

## Confirmation

On the target region, with 5,000 customers on the slowest GPU:
- the phase takes under 5 minutes, and each launch under 1 second;
- rows are bit-identical to CPU Dijkstra;
- time outside kernels is a small share on both Windows and Linux;
- the CPU contraction-hierarchy time is reported alongside.
