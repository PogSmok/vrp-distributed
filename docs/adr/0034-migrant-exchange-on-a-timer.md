# 0034. Islands exchange migrants on a 2-second timer

- **Status:** Accepted
- **Date:** 2026-11-24
- **Related:** 0004, 0018, 0021

## Context

ADR 0004 and 0018 had islands exchange migrants "every few iterations" or
"generations". A generation's length varies with the problem size, so a count
of generations gives no bound on control-plane traffic.

## Decision

Every 2 seconds, each island posts its best solution to the pool if it has
improved, and asks for a copy from a random other island. The pool keeps one
slot per island, and nobody waits for an answer. A migrant joins the
receiving island's population; it replaces nothing there directly.

## Alternatives considered

- **Every N generations (0004, 0018):** traffic grows as problems get
  smaller and generations get faster.
- **Overwriting the receiver's worst member:** removes diversity that
  survivor selection would have kept.

## Consequences

- Gain: at most one export and one request per island every 2 seconds, at any
  problem size.
- Cost: a fixed interval, not tuned per problem.
- Revisit: with the benchmarks of migration on, off and emptied (0004).

## Confirmation

Not yet confirmed: the benchmarks with migration on, off and emptied
(ADR 0004) are to compare the interval's cost and gain. `test_pool.py` and
`test_island.cpp` check only that migrants are kept apart by job and checked
on import.
