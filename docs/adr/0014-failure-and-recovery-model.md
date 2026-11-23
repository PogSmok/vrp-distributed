# 0014. Failure and recovery model

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** 0001, 0004, 0009, 0013, 0016, 0021, 0022

## Context

Any process or machine can fail during a job. We state what happens in each
case, and what is deliberately not recovered.

## Decision

- **Solve progress.** Every few seconds, each island's best solution and a
  convergence point are committed.
- **A worker is lost.** When its lease deadline passes, its non-island tasks
  are reassigned. Uploads are keyed by attempt, so a late one is ignored. A
  lost solve task's islands are dropped. If all islands are lost, SOLVE
  restarts from the
  stored matrix, seeded with the saved best solutions, at most once by
  default.
- **A worker reconnects.** The new session replaces the old one. Tasks the
  worker lists keep their leases; any others it held fail at once.
- **The orchestrator restarts.** It reloads its state and gives every lease
  one grace period. Workers re-register and resend reports that were not
  yet confirmed (0022). Tasks nobody reports return to `PENDING`, islands
  nobody reports are dropped, and the migrant pool restarts empty. A
  worker's longest reconnect backoff must be shorter than the grace period.
- **The database is down.** Heartbeats continue in memory. The orchestrator
  rejects new jobs, and transitions wait for the database (degraded mode,
  0021).
- **The object store is down.** Transfers retry with backoff, asking for
  fresh URLs when needed (0009); leases stay alive. A transfer still failing
  after 10 minutes fails its attempt, so that a store that never returns, or
  a wrong address for it, ends the job with a stored reason.
- **A stored object is later found corrupt.** The job fails with an explicit
  reason.
- **The client disconnects.** The job continues, and the client fetches the
  result later (0016).

## Alternatives considered

- **Checkpointing each island's solver state:** large, frequent writes; the
  matrix and the best solutions are enough to restart the search.
- **Failing the job when any worker is lost:** wastes work; the island model
  tolerates losing members.

## Consequences

- Gain: simple, testable rules, and no lost partitions.
- Cost: the orchestrator and the database are single points of failure; a
  restart briefly slows convergence; nothing is reassigned while the
  database is down.

## Confirmation

Fault-injection tests:
- kill, pause and disconnect workers in each phase;
- restart the orchestrator in each phase;
- stop the database and the object store for longer than their timeouts;
- corrupt a stored matrix chunk.

Each job finishes, or fails with a stored reason, and no healthy worker
loses its task.
