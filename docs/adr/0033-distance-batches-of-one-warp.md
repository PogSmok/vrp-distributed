# 0033. Distance batches hold one warp of 32 sources

- **Status:** Accepted
- **Date:** 2026-11-24
- **Related:** 0007, 0017, 0024

## Context

ADR 0017 planned to set the batch size `B` from free GPU memory. The kernel
gives each source one lane of a 32-thread warp, so one 32-bit word holds a
node's frontier flags for the whole batch.

## Decision

`B` is fixed at 32 (`DistanceBatch::kMaxSources`). A DISTANCE task runs its
rows in batches of 32 sources, in node order, so that nearby sources share a
batch. A batch takes 132 bytes of GPU memory per node and per stop, whatever
the GPU.

## Alternatives considered

- **`B` from free memory (0017):** several flag words per node and more
  kernel code, for a gain nobody has measured.
- **One source per thread, without warp sharing:** one edge load would no
  longer serve many sources.

## Consequences

- Gain: a simple kernel, and a memory need the Scheduler computes exactly
  (0007).
- Cost: a GPU with spare memory cannot run more sources at once.
- Revisit: with the benchmarks on the slowest GPU, together with ADR 0017's
  RPHAST fallback.

## Confirmation

GPU rows are bit-identical to CPU Dijkstra in `test_distance_batch.cpp` and
`test_distance_rows.cpp`.
