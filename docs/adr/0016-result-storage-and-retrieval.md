# 0016. Result storage and retrieval

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** 0001, 0009, 0014, 0015

## Context

A job produces final routes and a convergence history. Clients may disconnect
while a job runs, and the results must still be retrievable later. Routes
must be drawable on a map, so they need coordinates, not only node IDs.

## Decision drivers

- A disconnected client can still get its result.
- Large artifacts must not bloat the state database.
- Results stay interpretable (graph version recorded, see 0015).

## Decision

In the FINALIZE phase the orchestrator gives a worker the best stored
solution and the convergence history from the state database (0014). The
worker, which holds the graph, builds a result artifact and uploads it to the
object store under the job's ID through a presigned upload URL (0009). The
artifact contains the routes as node ID sequences and as coordinate
polylines, the total cost, the convergence history per island, the graph
version and the solver parameters. The orchestrator stores only metadata in
the state database (job ID, artifact location, summary cost, creation and
expiry time). While
connected, the client receives progress over its stream. At any later time it
asks the orchestrator for a job's result and receives a presigned download
URL (0009). Artifacts expire after a configurable retention period, with a
default of 30 days, which is an assumption to confirm. The orchestrator
removes expired artifacts.

## Alternatives considered

### Option A: Store results in the state database
- Pros: one store; transactional with job metadata.
- Cons: route polylines can be large, bloating the database and its backups.
- Why rejected: large artifacts belong in the object store. The small inputs
  (best solutions, convergence points) are already in the database for
  recovery (0014), and the artifact is built from them.

### Option B: Return results only through the gRPC stream
- Pros: simplest, nothing to store.
- Cons: the result is lost if the client is not connected at that moment.
- Why rejected: long jobs and unreliable clients make this fragile.

## Consequences

**What we gain:**
- Reconnecting clients can fetch results; the database stays small.

**What we pay for it:**
- A cleanup process and one more artifact format to version.

**Risks / to revisit later:**
- Large polylines for very big problems; consider simplifying geometry.

## Confirmation

Disconnect the client during a job, reconnect after completion and retrieve
the result. Verify that an expired result is no longer downloadable.
