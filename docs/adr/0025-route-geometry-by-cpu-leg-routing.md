# 0025. Route geometry: legs re-routed on the CPU in FINALIZE

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** 0011, 0016, 0018, 0020, 0023, 0024

## Context

Results need road polylines, but the distance phase keeps only costs, and
storing shortest-path trees would take gigabytes. FINALIZE must therefore
re-route every leg: about 5,000 legs for 5,000 customers.

## Decision

FINALIZE first checks the solution: every customer appears exactly once, no
route exceeds the vehicle capacity, and there are no more routes than
vehicles. It then routes each leg with a bidirectional Dijkstra search on
the CPU, in `vrp-core`, in batches on a thread pool the worker supplies. The
summed cost must equal the stored best cost exactly, because costs are
integers (0020). Islands report only feasible solutions (0018), so any
failed check is a bug and fails the task.

## Alternatives considered

- **Storing predecessors during the distance phase:** gigabytes per job.
- **Running the GPU many-source kernel again with predecessors:** costs as
  much as the distance phase, for one target per source.
- **Splitting leg routing across workers:** revisit if FINALIZE misses its
  target.
- **Straight lines between stops:** routes cannot be drawn on the roads.

## Consequences

- Gain: FINALIZE leaves the GPU free and checks the result.
- Cost: a second shortest-path implementation to maintain.
- Revisit: contraction hierarchies, if CPU routing is too slow.

## Confirmation

FINALIZE for 5,000 customers meets its target on the slowest worker. The
re-routed costs match the solver's, and invalid solutions fail with
explicit reasons.
