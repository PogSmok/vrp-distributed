# 0003. Orchestrator as the single owner of job state

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** 0001, 0010, 0012

## Context

Clients, workers and monitoring all need job information. If each read the
database, its schema would become a contract shared by all of them.

## Decision

Only the orchestrator reads and writes the state store; every other
component uses the orchestrator's gRPC interface. The one exception is
`postgres_exporter` (0010), whose read-only role sees database statistics
but not job tables.

## Alternatives considered

- **Workers access the database directly:** saves one hop but couples every
  worker to the schema, and the dispatch rate is too low to need it.
- **A separate state or API service:** an extra service and contract
  without a second type of client to justify it (0012).

## Consequences

- Gain: the store can change without touching workers or clients; one place
  for validation and authorisation.
- Cost: the orchestrator is on every control path and is a single point of
  failure for scheduling.

## Confirmation

Review check: only the orchestrator has read-write database credentials,
and the monitoring role cannot read job tables.
