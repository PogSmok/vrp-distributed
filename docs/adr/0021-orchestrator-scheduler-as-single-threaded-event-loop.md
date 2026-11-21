# 0021. Orchestrator core: single-threaded, event-driven scheduler

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** 0001, 0004, 0013, 0014, 0022, 0026

## Context

Many concurrent gRPC calls touch the same scheduling state, and the
scheduling rules are the most intricate logic in the system. They must be
testable without real time, workers or a database. The load is small: at
most 8 workers and one running job.

## Decision

- **One owner.** The Scheduler is one asyncio task that handles the events
  in its queue one at a time (0026). It owns all job, task, lease, group and
  catalogue state. Handlers only post events and await the reply. Timer
  ticks come from an injected clock.
- **Shared loop.** Every handler, the State Repository and the Migrant Pool
  run on the same event loop. No code may block or do heavy CPU work on it;
  such work goes to `asyncio.to_thread`.
- **Commit before apply.** The State Repository commits each transition in
  its own task. The Scheduler applies the transition, and sends its
  messages, only after the commit returns (0022); the entities it touches
  wait until then.
  - If the database is down, commits are retried and the Scheduler degrades
    (it rejects new jobs and queues transitions).
  - Any other commit error is a bug, and the process exits.
- **No waiting on I/O in the loop.**
  - Each session has a bounded send queue; overflowing it closes the
    session.
  - Progress polls are answered from memory and rate-limited per client.
  - Graph verification, deletions and upload checks run outside the loop
    and come back as events. An upload check compares the object's size and
    ETag with the reported MD5, before the task becomes `DONE`.
- **One exception.** Migrant traffic, the bulk of all messages, bypasses
  the Scheduler's queue: the Worker API uses the Migrant Pool directly. The
  Scheduler creates and removes a pool for each solve phase, and registers
  and removes its islands. The pool ignores migrants from islands it does
  not know.

## Alternatives considered

- **Shared state behind locks:** races and lock-ordering bugs in group
  scheduling, and hard to test.
- **A stateless orchestrator, with all state in the database:** every
  heartbeat becomes a write, while streams and the pool stay in memory
  anyway.
- **One actor per job:** jobs share workers, and only one job runs at a
  time.

## Consequences

- Gain: sequential, lock-free scheduling that can be tested as a pure state
  machine; no peer can stall the others.
- Cost: a queue hop per call and a commit round trip per change; degraded
  mode to test.
- Revisit: batch several transitions per commit, if commit latency limits
  throughput.

## Confirmation

Unit tests run without gRPC, a database or real time. They cover group
start, lease expiry, stale attempts, failed upload checks and commits, a
restart, and a database outage. A hanging database does not delay
heartbeats, a client polling fast does not delay assignments, and 8
simulated workers keep the event queue short.
