# 0004. Island migration via orchestrator-held migrant pool

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** 0002, 0003, 0011, 0013, 0014

## Context

The VRP solver uses an island model: each worker evolves its own population
and occasionally exchanges its best solutions with the others. A migrant is
a single solution (a permutation of customers, a few kilobytes) exchanged
rarely (every N iterations). Workers should not need to know how many peers
exist. Exchange of the distance matrix is a different problem and is handled
in 0011.

## Decision drivers

- Migration payloads are small and infrequent.
- Islands may be lost during a run (0014) without failing the job.
- No peer discovery or fixed membership.

## Decision

Workers push their best solutions to a per-job migrant pool held in memory by
the orchestrator and pull random migrants from that pool when they are ready.
There is no direct worker-to-worker migration traffic.

## Alternatives considered

### Option A: Direct peer-to-peer exchange (NCCL / MPI)
- Pros: lowest latency, orchestrator not involved.
- Cons: requires fixed membership and peer discovery; a dying participant can
  stall or break collective operations.
- Why rejected: fragile with dynamic membership, and the payloads are small.

### Option B: Orchestrator broadcast to all workers
- Pros: simple mental model.
- Cons: N-squared message fan-out; push semantics conflict with 0002.
- Why rejected: a pull-based pool is cheaper.

### Option C: Persist the migrant pool in the state database
- Pros: the pool survives an orchestrator restart.
- Cons: constant write load for data that is expendable by design.
- Why rejected: losing the pool only means islands evolve alone for a while
  (see 0014). Each island's best solution *is* persisted, but at a much
  lower rate and for recovery, not for migration (0014).

## Consequences

**What we gain:**
- Workers do not know about each other; islands can leave during a run
  without affecting the others (the group is not enlarged, see 0013).
- Graceful degradation: without the pool, islands keep evolving independently.

**What we pay for it:**
- The orchestrator is on the migration path and adds one hop of latency.
- Pool size and replacement policy are the orchestrator's responsibility.

**Risks / to revisit later:**
- The pool is lost when the orchestrator restarts.
- Migration frequency and payload size are assumptions; revisit if the
  measured values are much larger.

## Confirmation

Benchmark solution quality and time-to-target with migration enabled and
disabled, and with the pool deliberately emptied mid-run.
