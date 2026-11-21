# 0020. Edge cost: road length and travel time stored, chosen per job

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** 0005, 0006, 0011, 0015, 0025

## Context

The objective is a sum of edge weights. Travel time is more meaningful than
length, but needs a speed for every road, and OSM often lacks `maxspeed`.
The choice shapes the graph, problem and matrix formats.

## Decision

Each edge stores its length in whole metres and its travel time in tenths
of a second, both as unsigned 32-bit integers. Travel time comes from
`maxspeed`, or else from a versioned default speed for the road class. Each
problem picks one of the two, and only that weight is loaded onto the GPU.
Costs are integers everywhere (32-bit matrix entries, 64-bit route sums), so
GPU and CPU results are bit-identical and every check is exact.

## Alternatives considered

- **Length only:** ignores road class.
- **Travel time only:** a second weight costs only 4 bytes per edge.
- **Time-dependent (traffic) costs:** no data, and much harder on a GPU.
- **Floating-point costs:** the result depends on the order of addition, so
  checks would need tolerances.

## Consequences

- Gain: both objectives from one import; exact, reproducible costs.
- Cost: a speed table to maintain; no turn costs or traffic.

## Confirmation

Every edge has both weights, and an edge without `maxspeed` gets its road
class's default. A matrix computed twice on the GPU and once on the CPU is
bit-identical.
