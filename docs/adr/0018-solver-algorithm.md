# 0018. Solver algorithm: memetic search on giant tours, local search on the GPU

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** 0004, 0006, 0011, 0013, 0020, 0023, 0024

## Context

Each island solves a fixed-fleet capacitated VRP with up to 5,000 customers
on an asymmetric integer matrix, in 1 to 10 minutes, in steps under a
second. The best published heuristic, HGS-CVRP, is memetic, but runs its
local search sequentially on one CPU core.

## Decision

Each island runs an HGS-style memetic algorithm on giant tours.
- **CPU part.** Tournament selection, order crossover, a limited-fleet Split
  into routes, capacity violations allowed with an adaptive penalty, and
  survival by cost and diversity.
- **GPU part.** Local search on a batch of children. It evaluates every
  granular move in parallel (relocate, swap, or-opt, 2-opt* and intra-route
  2-opt, over each customer's 20 to 40 nearest neighbours). It then applies
  the best improving moves that share no route, and repeats until none
  improve.
- **Asymmetry.** Per-route prefix sums of forward and reverse costs keep
  segment reversals constant-time.
- **Feasibility.** Infeasible children take part in the search, but an
  island reports, and exports as migrants, only feasible solutions. An
  island that has none reports nothing (0013).
- **Islands.** A solve task runs P islands on its GPU (0013), and every
  island exchanges migrants through the pool every few generations (0004).
  Seeds come from the job's seed and the island index.

## Alternatives considered

- **HGS-CVRP ported unchanged:** its sequential local search offers little
  parallel work; it is kept as the CPU quality reference.
- **Simulated annealing or tabu search with GPU moves:** weaker on large
  CVRP.
- **Ant colony optimisation:** weaker solution quality.
- **Neural heuristics:** out of scope.

## Consequences

- Gain: a proven structure, with its expensive part on the GPU; it fits the
  migrant format and the step model.
- Cost: a CPU part and a GPU part to keep consistent; many tuning
  parameters; parallel rounds may converge differently from sequential
  search.
- Revisit: move Split to the GPU if the CPU becomes the bottleneck.

## Confirmation

Report gaps to best known solutions (median of 5 seeds, fixed time budget):
- on CVRPLIB set X, the Belgium instances and asymmetric instances, compared
  with HGS-CVRP given the same budget;
- on road problems, compared with a run given ten times the budget.

Costs from the GPU match the CPU re-evaluation, and no step exceeds 1
second. No infeasible solution is ever reported or exported.
