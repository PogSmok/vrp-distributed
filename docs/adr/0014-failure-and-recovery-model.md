# 0014. Failure and recovery model

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** 0001, 0002, 0004, 0011, 0013, 0016

## Context

The system runs on several PCs. Workers, the orchestrator, the state
database, the object store and clients can fail or disconnect during a job.
We need to state what happens in each case and what is deliberately not
recovered.

## Decision drivers

- No loss of accepted jobs.
- Islands are expendable; matrices and results are not.
- Simple, testable recovery rules.

## Decision

- **Solve progress is persisted.** Every N iterations each island reports
  its best solution and a convergence point (iteration, time, best cost). The
  orchestrator stores the latest best solution per island and the
  convergence points in the state database. These are a few kilobytes per
  island, far less than the migrant traffic (0004).
- **Worker dies or disconnects.** Heartbeats stop or the stream breaks, and
  the lease expires. Prepare, distance and finalize tasks return to
  `PENDING` and are reassigned. Uploaded objects are keyed by task ID and
  attempt number, and the orchestrator records which attempt completed, so a
  late upload from an expired attempt is ignored (0013). A lost island is
  dropped: the job continues with the remaining islands. If all islands are
  lost, the solve phase is restarted from the stored matrix, with the new
  islands seeded from the persisted best solutions. Because every worker
  holds the full graph (0011), any worker can take over.
- **Orchestrator restarts.** All job, task, lease, catalogue and solve
  progress state is in the state database (0001). On start the orchestrator
  reloads it. Workers reconnect with backoff, re-register and report the
  tasks they are running; leased tasks that are not reported after a grace
  period return to `PENDING`. The in-memory migrant pool is lost; islands
  continue without migration and refill the pool (0004).
- **State database unavailable.** The orchestrator rejects new jobs and
  pauses lease expiry, because heartbeats cannot be recorded. When the
  database returns, all active leases are extended by a grace period before
  expiry checks resume, so healthy workers are not mistaken for dead ones.
  Running islands continue with data already loaded.
- **Object store unavailable.** Uploads of matrix rows and results are
  retried with backoff; tasks stay leased while their worker keeps sending
  heartbeats.
- **Client disconnects.** The job continues. The client retrieves the result
  later (0016).

Heartbeat interval, lease timeout, grace periods, the reporting interval N
and retry limits are configuration values to be tuned by measurement.

## Alternatives considered

### Option A: Persist and checkpoint the solver state of every island
- Pros: a lost island resumes exactly where it stopped.
- Cons: large, frequent writes; complex to implement on the GPU.
- Why rejected: islands are cheap to lose; the matrix and each island's best
  solution are already persisted, which is enough to restart the search.

### Option B: Fail the whole job when any worker is lost
- Pros: simplest rule.
- Cons: wastes work and makes long jobs fragile.
- Why rejected: the island model tolerates the loss of members by design.

## Consequences

**What we gain:**
- Clear, testable behaviour for each failure, and no partition-loss case.

**What we pay for it:**
- The orchestrator and state database remain single points of failure.
- A restart loses the migrant pool and briefly slows convergence.
- A small, steady write load on the state database from progress reports.
- While the state database is down, lease expiry is paused, so a worker that
  really died is only detected after the database returns.

**Risks / to revisit later:**
- Orchestrator high availability is out of scope for now.

## Confirmation

Fault-injection tests in the benchmark suite: kill a worker in each phase,
restart the orchestrator in each phase, stop the state database for longer
than the lease timeout, pause a worker until its lease expires and then
resume it, and disconnect the client. A job must still finish, or fail with
a stored, explicit reason. After an orchestrator restart during SOLVE, the
final result must be no worse than the best cost stored before the restart.
