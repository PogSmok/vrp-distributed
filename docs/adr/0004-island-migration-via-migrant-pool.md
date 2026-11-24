# 0004. Island migration via orchestrator-held migrant pool

- **Status:** Accepted; exchange interval set by 0034
- **Date:** 2026-11-21
- **Related:** 0011, 0013, 0014, 0018, 0021

## Context

Islands exchange their best solutions (migrants: giant tours of about 20 KB
at 5,000 customers) every few iterations. Islands can be lost during a run,
and workers should not need to know their peers.

## Decision

Islands push migrants to a per-job pool held in the orchestrator's memory,
and pull random migrants from it when they are ready. A receiving island
checks that a migrant visits every customer exactly once and recomputes its
cost from its own matrix; it drops invalid migrants. The pool accepts
migrants only from the islands currently registered for the job (0021).

## Alternatives considered

- **Peer-to-peer exchange (NCCL, MPI):** needs fixed membership, and a dying
  peer stalls collective operations.
- **Broadcast by the orchestrator:** islands run at different speeds, so
  pushed migrants arrive when the receiver is not ready for them.
- **A persisted pool:** one write per migrant for expendable data; recovery
  needs only each island's best solution, which 0014 stores.

## Consequences

- Gain: islands are independent of each other and keep evolving if the pool
  is lost.
- Cost: the orchestrator is on the migration path; the pool is lost on
  restart.
- Revisit: direct exchange, if the hop through the orchestrator dominates
  iteration time.

## Confirmation

Benchmark with migration enabled, disabled, and with the pool emptied
mid-run. Inject an invalid migrant, and one from an unknown island, and
check that both are dropped.
