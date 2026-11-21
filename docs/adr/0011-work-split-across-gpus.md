# 0011. Work split across GPUs: full graph per worker, two-phase pipeline

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** 0004, 0005, 0007, 0013, 0014

## Context

A complete VRP solution needs distances between every pair of customers.
With a graph partitioned across workers, a worker holding partition A would
need shortest paths into partition B, which requires boundary nodes or an
overlay graph. That is the hardest problem in the system, and it is only
necessary if the graph does not fit on one GPU. A simplified drivable graph
of a country the size of Poland in a compact array format is estimated to be
in the range of a few hundred megabytes. This is an estimate, not a
measurement, and it must be confirmed.

## Decision drivers

- Correctness of customer-to-customer distances.
- Distribution must be real and measurable (scaling from 1 to N GPUs).
- Any worker must be able to replace any other (0014).

## Decision

Every worker loads the whole regional graph into GPU memory. Work is split in
two phases:

1. **Distance phase.** The customers are split into chunks of source
   customers. Each task computes shortest-path distances from its sources to
   all customers on the worker's GPU, and uploads the resulting matrix rows to
   the object store through a presigned upload URL (0009).
2. **Solve phase.** Every island downloads the complete matrix (rows
   assembled by chunk) and runs the island solver. Road distances are
   asymmetric (one-way streets), so the solver must not assume a symmetric
   matrix. Migration uses the orchestrator's migrant pool (0004).

Distribution comes from splitting sources in phase 1 and from running
islands in phase 2, not from partitioning the graph. For `n` customers the
matrix is `n x n` single-precision values, which is `4 * n^2` bytes: about
100 MB for 5,000 customers and 400 MB for 10,000. A pair with no route is
stored as +infinity; because the graph is strongly connected (0005), such a
value indicates a bug, and the distance task fails instead of uploading it.

## Alternatives considered

### Option A: Partitioned graph with boundary nodes or an overlay
- Pros: supports graphs larger than one GPU; demonstrates more distribution.
- Cons: cross-partition shortest paths need boundary-node distance tables or
  a multi-level hierarchy; a failed worker can mean a lost partition.
- Why rejected: unnecessary if the graph fits on one GPU. Revisit with a new
  ADR if it does not.

### Option B: Compute the whole matrix on one GPU
- Pros: simplest.
- Cons: phase 1 becomes a serial bottleneck and removes the embarrassingly
  parallel part of the scaling story.
- Why rejected: defeats the purpose of measuring GPU scaling.

### Option C: Exchange matrix rows directly between workers (NCCL / MPI)
- Pros: lower latency than going through storage.
- Cons: needs fixed membership and peer discovery (see 0004).
- Why rejected: the object store keeps membership dynamic and the matrices
  are small enough.

## Consequences

**What we gain:**
- A simple, correct algorithm; phase 1 is expected to scale close to
  linearly (a hypothesis to test).
- No partition-loss failure mode: every worker is replaceable.

**What we pay for it:**
- Each GPU must hold the graph, the matrix and the solver state, so graph
  size is limited by the smallest GPU.
- Matrix rows travel through the object store.

**Risks / to revisit later:**
- If the target region does not fit on the smallest GPU, graph partitioning
  needs its own ADR.

## Confirmation

Measure the compact graph size for the target region and confirm that it fits
on the smallest GPU (see `docs/quality-attributes.md`) with room for the
matrix and solver state. If it does not, a graph-partitioning ADR supersedes
this one.

Benchmark phase-1 speedup from 1 to N GPUs with warm graph caches; graph
download and load time are measured separately. Measure object-store
transfer time for a 5,000-customer matrix.

The solver is stochastic, so every solve-phase measurement is repeated with
at least 5 seeds and reported as a median with its spread. The 1-GPU baseline
runs the same total number of islands as the N-GPU run (as several
populations on one GPU), so a speedup comes from the extra GPUs and not from
extra islands.
