# 0003. Orchestrator as the single owner of job state

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** 0001, 0002, 0010, 0012

## Context

Several components need job information: the client wants status and
progress, workers want assignments, and monitoring wants queue length. If
each component read the database directly, its schema would become a shared
contract between all of them.

## Decision drivers

- The state store must be replaceable without touching workers or clients.
- One place enforces validation, authorisation and invariants.
- Job dispatch is coarse-grained (seconds to minutes per task), so an extra
  hop on the control path is cheap.

## Decision

The orchestrator is the only component that reads and writes the state store
(0001). Clients and workers use the orchestrator's gRPC interface to submit
jobs, receive assignments, report heartbeats and results, and query status.

The one exception is monitoring: `postgres_exporter` (0010) connects with a
separate read-only role that can see database statistics, not job tables.

## Alternatives considered

### Option A: Workers access the database directly
- Pros: one hop fewer; the orchestrator is off the dispatch path.
- Cons: the schema is a shared integration contract; changing the store
  affects every worker; authorisation moves into the database.
- Why rejected: dispatch rate is low, so the saved hop is not worth the
  coupling.

### Option B: A separate stateless state/API service in front of the database
- Pros: keeps the orchestrator free of client-facing code; scales separately.
- Cons: an extra service and contract (see 0012).
- Why rejected: the orchestrator already owns scheduling; a second service
  adds complexity without a second client type to justify it.

## Consequences

**What we gain:**
- The state store can be replaced without changing workers or clients.
- A single place for validation and authorisation.

**What we pay for it:**
- The orchestrator is on the control path of every job and is a single
  point of failure for scheduling.

**Risks / to revisit later:**
- If dispatch rate becomes high, revisit direct access for workers.

## Confirmation

Review check: no component other than the orchestrator has read-write
database credentials, and the monitoring role cannot select from job tables.
In the architecture model, only the orchestrator and Prometheus have
relations to the state store.
