# 0011. Work split across GPUs: full graph per worker, two-phase pipeline

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** 0004, 0005, 0007, 0013, 0017, 0018, 0020

## Context

Distances between customers on a partitioned graph need boundary nodes or
an overlay. That is the hardest problem in the system, and it is needed only
if the graph does not fit on one GPU. A compact country-sized graph is
estimated at a few hundred MB, which still has to be measured.

## Decision

Every worker holds the whole regional graph.
- **Distance phase:** the stops (depot and customers) are split into chunks
  of sources, one task per chunk. Each task uploads its matrix rows through
  presigned URLs.
- **Solve phase:** islands run on the full matrix.

The matrix is `(n+1) × (n+1)` unsigned 32-bit costs (about 100 MB at 5,000
customers) with the depot as row and column 0. It is asymmetric, and a pair
with no route is treated as a bug.

## Alternatives considered

- **A partitioned graph:** cross-partition paths are hard, and a lost worker
  loses a partition; revisit only if the graph does not fit.
- **The whole matrix on one GPU:** removes the parallel part of the scaling
  story.
- **Exchanging rows between workers directly:** needs fixed membership.

## Consequences

- Gain: a simple, correct design in which any worker can replace any other.
- Cost: graph size is limited by the smallest GPU; matrix rows travel
  through the object store.

## Confirmation

Measure the graph against the smallest GPU. Benchmark the distance phase's
speedup from 1 to N GPUs with warm caches. Repeat every solve-phase
measurement with at least 5 seeds, against a 1-GPU job that runs the same
total number of islands in one task (0013).
