# 0001. Job and state store: PostgreSQL

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** 0003, 0013, 0014, 0021, 0022

## Context

The orchestrator needs durable, transactional and queryable storage for
jobs, tasks, assignments, the graph catalogue, solve progress and result
metadata, and rebuilds its state from it after a restart (0014).

## Decision

PostgreSQL is the system of record. The Scheduler keeps the working state in
memory and commits every transition before applying it (0021, 0022); the
database is read in full only at start-up. Lease deadlines and the migrant
pool are not stored.

## Alternatives considered

- **SQLite in the orchestrator:** the closest option (no network failure
  mode), rejected because a separate database keeps a clear boundary and its
  own tooling, allows remote SQL analysis of benchmarks, and can later be
  replicated or moved to its own host without a migration.
- **Redis Streams or Kafka:** queue or log semantics; we need queryable
  state.

## Consequences

- Gain: transactions, a real schema and ad-hoc queries.
- Cost: one more service, schema migrations, and the database-outage
  handling in 0014; a single point of failure.
- Revisit: add replication if availability becomes a requirement.

## Confirmation

Restart the orchestrator in the middle of a job and check that the job
resumes from the stored state, and that each assignment and its task state
change are committed in one transaction.
