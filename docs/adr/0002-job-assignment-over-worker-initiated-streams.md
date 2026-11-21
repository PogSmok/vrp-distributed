# 0002. Job assignment over worker-initiated gRPC streams

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** 0003, 0008, 0010, 0013, 0014

## Context

Workers on several machines join and leave at any time, the solve phase
must start a group of islands together, and dead workers must be detected
quickly.

## Decision

Each worker opens a long-lived bidirectional gRPC stream to the orchestrator
and registers over it with its GPU model and memory, driver, build and
protocol version, and metrics address. Over the stream it announces when it
is idle, sends heartbeats and log lines (0010), and receives leased
assignments. The orchestrator
never opens connections to workers.

## Alternatives considered

- **The orchestrator pushes work to workers:** needs worker addresses and
  handling of unreachable workers.
- **Workers poll for work:** cannot start a group together, and adds
  latency.

## Consequences

- Gain: workers join without configuration; group start; a dead worker is
  noticed when its stream breaks or its heartbeats stop.
- Cost: one open stream per worker; reconnect logic in the worker.

## Confirmation

Start three workers without changing any configuration and check that all
receive work. Cut one worker's connection and check that its task is
reassigned.
