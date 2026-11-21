# 0001. Job and state store: PostgreSQL

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** 0003, 0013, 0014, 0015, 0016

## Context

The orchestrator needs durable storage for jobs, tasks, worker leases, the
catalogue of graph versions and result metadata. A job is not one message
consumed by one worker: it is a set of tasks plus a group of islands that
must start together (0013). Only the orchestrator accesses the store (0003),
and it must recover its state after a restart (0014).

## Decision drivers

- Job state survives orchestrator restarts.
- Lease and task transitions are transactional.
- Relational queries are needed (jobs by status, results by graph version).
- Low operational cost for a small number of machines.

## Decision

PostgreSQL is the system of record for all orchestrator state. Tasks are rows
in a table, and the orchestrator selects ready tasks with
`FOR UPDATE SKIP LOCKED`. This keeps task selection safe under concurrency,
but it does not by itself allow a second orchestrator: the migrant pool
(0004) and the worker streams (0002) belong to one process.

## Alternatives considered

### Option A: Redis Streams
- Pros: consumer groups, acknowledgement and take-over of stalled entries
  built in; very fast.
- Cons: with a single consumer (the orchestrator) the consumer-group
  features add little; the catalogue and result metadata are awkward as
  key-value data; durability depends on AOF configuration.
- Why rejected: queue semantics are not the real need. Durable, queryable
  state is.

### Option B: SQLite embedded in the orchestrator
- Pros: no extra service; simplest to run.
- Cons: state is tied to one host and one process; no concurrent access from
  admin tools or a future second orchestrator.
- Why rejected: PostgreSQL costs one container and keeps options open. SQLite
  would also be acceptable for a strictly single-orchestrator deployment.

### Option C: Apache Kafka
- Pros: very durable log, mature ecosystem.
- Cons: classic consumer groups conflict with long-running jobs (poll
  limits, rebalances); newer queue-style share groups (KIP-932) address
  this, but job state would still need a separate queryable store.
- Why rejected: state, not a log, is the real need, and Kafka is heavy for
  this scale.

## Consequences

**What we gain:**
- Transactions for leases, a real schema, and ad-hoc queries for debugging.

**What we pay for it:**
- One more service to run, schema migrations, and a C++ client library.
- PostgreSQL is a single point of failure in the initial deployment.

**Risks / to revisit later:**
- Add replication if availability becomes a requirement.

## Confirmation

Restart the orchestrator in the middle of a job (see 0014) and verify that
the job resumes from the stored state. Verify that a lease update and the
task state change happen in one transaction.
