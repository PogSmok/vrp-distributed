# 0013. Job and task model with group scheduling

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** 0001, 0002, 0004, 0011, 0014

## Context

A job is not a single unit of work consumed by one worker. It has phases with
different scheduling needs: independent tasks (distance computation) and a
group of islands that must run at the same time and exchange migrants.

## Decision drivers

- Islands must start together to exchange migrants.
- Distance tasks are independent and idempotent.
- A worker's GPU runs one task at a time.
- Every transition must be recoverable from the state store (0001).

## Decision

A job moves through four phases, driven by the orchestrator:

1. **PREPARE:** one task snaps customer coordinates to graph nodes of the
   job's pinned graph version (0015).
2. **DISTANCE:** N independent tasks, one per chunk of source customers (0011).
3. **SOLVE:** one group of K islands, assigned only when K workers are idle.
   If fewer than K are idle after a wait timeout, the job starts with the
   number available. Islands share a job-scoped migrant pool (0004). The
   group is fixed once started: lost islands are not replaced and workers
   that become idle later do not join it, so benchmark runs stay comparable.
   The phase ends when the job's time limit is reached or the best cost has
   not improved for a configured period, whichever comes first; the
   orchestrator then tells the islands to stop.
4. **FINALIZE:** the best solution is selected and a result artifact is
   produced (0016).

Tasks are `PENDING`, `LEASED`, `RUNNING`, `DONE` or `FAILED`. A task is
leased to a worker with an expiry that heartbeats renew; an expired lease
returns the task to `PENDING`, up to a maximum number of attempts. Every
lease carries an attempt number that increases on each reassignment. The
orchestrator accepts heartbeats, completions and uploads only for the
current attempt, so a worker whose lease has already expired cannot overwrite
the result of its replacement. Jobs are scheduled first-in, first-out.

## Alternatives considered

### Option A: Treat every island as an independent queue task
- Pros: one uniform mechanism for all work.
- Cons: nothing ensures that islands run concurrently, so migration may find
  an empty pool or finished peers.
- Why rejected: concurrency of islands is a requirement.

### Option B: A coordinator worker spawns and manages its peers
- Pros: scheduling logic for a job lives in one place.
- Cons: moves scheduling into workers and bypasses the orchestrator's state.
- Why rejected: scheduling and recovery belong with the orchestrator.

## Consequences

**What we gain:**
- Clear phase boundaries, idempotent retries for distance tasks, and
  explicit group start.

**What we pay for it:**
- The scheduler is more complex than a plain queue, and head-of-line
  blocking is possible with first-in, first-out scheduling.

**Risks / to revisit later:**
- Wait timeout, group-size policy, stopping criteria and priorities need
  tuning.
- Replacing lost islands with newly idle workers could improve robustness;
  revisit if long jobs lose many islands.

## Confirmation

With three workers, a job requesting three islands starts the solve phase
only when all three are idle. Killing a worker during the distance phase
still lets the job finish. A completion sent with an old attempt number is
rejected.
