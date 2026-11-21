# 0002. Job assignment over worker-initiated gRPC streams

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** 0003, 0008, 0013, 0014

## Context

Workers run on several machines with different GPUs, and the set of workers
changes over time. Jobs have phases: independent distance tasks, and a group
of islands that must start together (0013). Plain pull-based polling does not
coordinate a group start, and plain push needs the orchestrator to know every
worker's address.

## Decision drivers

- New workers join without orchestrator configuration.
- The solve phase needs a group of workers to start together.
- Fast detection of dead workers.
- The orchestrator never needs to know worker addresses.

## Decision

After registering, each worker opens a long-lived, worker-initiated
bidirectional gRPC stream to the orchestrator. The worker announces when it
is idle and sends heartbeats over the stream. The orchestrator sends leased
assignments (a distance task, or membership in an island group) down the same
stream. The orchestrator never opens connections to workers.

## Alternatives considered

### Option A: Pure push (orchestrator calls workers)
- Pros: the orchestrator controls exactly when and where work runs.
- Cons: needs a registry of reachable worker addresses and handling of
  unreachable workers.
- Why rejected: more state and failure handling in the orchestrator, and
  workers would need listening ports.

### Option B: Pure polling pull (worker repeatedly asks "any job?")
- Pros: simplest model; natural load balancing.
- Cons: cannot start a group of islands together without an extra
  rendezvous mechanism; polling adds latency.
- Why rejected: group scheduling is a core requirement.

Workers reading tasks directly from the database is covered in 0003.

## Consequences

**What we gain:**
- Elastic membership, load balancing (idle workers are offered work), and
  group start (the orchestrator waits until enough workers are idle).
- Dead workers are detected when the stream breaks or heartbeats stop.

**What we pay for it:**
- The orchestrator holds one open stream per worker.
- Reconnection and backoff logic is needed in the worker.

**Risks / to revisit later:**
- This ADR does not provide NAT traversal for metrics: Prometheus scrapes
  workers directly, so workers are assumed to be on the same network as
  Prometheus (see 0010).

## Confirmation

Start three workers without changing orchestrator configuration and verify
that all receive work. Cut one worker's connection and verify that its lease
expires and its task is reassigned (see 0014).
