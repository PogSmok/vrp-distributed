# 0013. Job and task model with group scheduling

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** 0001, 0004, 0009, 0011, 0014, 0016, 0018, 0021, 0025

## Context

A job has phases with different scheduling needs: independent distance
tasks, and a group of islands that must run at the same time. Every
assignment must survive a restart, but heartbeats must not cost database
writes.

## Decision

- **Phases.**
  1. PREPARE: one task snaps stops to the job's pinned graph.
  2. DISTANCE: one task per chunk of sources.
  3. SOLVE: K leased tasks, each running P islands on its GPU. K and P are
     job parameters; K defaults to the number of registered workers and P
     to 1. A 1-GPU baseline is an ordinary job with K = 1, on the same code
     path. Tasks start when K workers are idle (or with fewer after a wait
     timeout), and the group is fixed once it starts. SOLVE ends at the
     time limit, or when the best cost has not improved for a set period.
     The islands then send their final best solutions, which are committed
     before FINALIZE.
  4. FINALIZE: the best of every island's last saved solution, including
     lost islands, is checked, routed and stored (0016, 0025). Islands
     report only feasible solutions (0018). If none has found one, the job
     ends `FAILED` with "no feasible solution found": an expected outcome,
     not a bug.
- **Tasks and leases.** A task is `PENDING`, `LEASED`, `DONE` or `FAILED`.
  A lease is one attempt by one worker, identified by its token name (0009).
  The assignment (worker, attempt, build version, GPU) is stored. The
  deadline is kept only in memory, and heartbeats extend it.
- **Expiry.** An expired task returns to `PENDING` with the next attempt
  number, up to a limit, after which it fails the job. An expired solve
  task fails only itself and its islands.
- **Reports.** Only the worker assigned to the current attempt can report.
  A task with uploads becomes `DONE` only after the upload check (0021).
- **Jobs.** Jobs run first-in, first-out. A client can cancel its own job:
  tasks stop at once, FINALIZE does not run, and the job ends `CANCELLED`
  without a result.

## Alternatives considered

- **Islands as independent queue tasks:** nothing would make them run at
  the same time.
- **A coordinator worker that manages its peers:** moves scheduling out of
  the orchestrator.

## Consequences

- Gain: clear phases, idempotent retries and an explicit group start.
- Cost: more complex than a plain queue; first-in, first-out scheduling can
  block the queue behind one job.
- Revisit: group-size policy and stopping criteria; replacing lost islands.

## Confirmation

The solve phase starts only when K workers are idle. Killing a worker
during DISTANCE still lets the job finish. Reports for an old attempt, or
from another worker, are rejected. Heartbeats cause no database writes.
Cancelling a job frees its workers within seconds. A problem that passes
the submission checks, but whose demands cannot be packed into the fleet,
ends with "no feasible solution found".
