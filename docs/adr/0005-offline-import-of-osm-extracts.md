# 0005. Offline import of regional OSM extracts

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** 0007, 0011, 0015, 0020

## Context

The solver needs a road graph from OpenStreetMap in a compact GPU format,
and benchmarks must be reproducible. Raw OSM graphs contain nodes that
cannot be reached or left.

## Decision

A Python preprocessor (pyrosm) imports regional `.pbf` extracts offline,
keeps the largest strongly connected component of the drivable graph, and
stores length and travel time for every edge (0020). The running system
never queries OSM.

## Alternatives considered

- **A live API (Overpass):** rate-limited, a runtime dependency, and results
  change between runs.
- **The whole planet:** far larger than one GPU can hold.
- **Routing-engine data (OSRM, Valhalla):** formats our kernels cannot use,
  and it hides the work we want to show.

## Consequences

- Gain: reproducible benchmarks with no runtime dependency; graph size is a
  benchmark axis.
- Cost: data is stale until the next import; customers outside the largest
  component snap further away; OSM's ODbL licence requires attribution.
- Revisit: switch to pyosmium if pyrosm stops supporting our Python version
  or runs out of memory.

## Confirmation

Preprocess the target region, check that the graph fits on the smallest
GPU, and check that shortest paths from random nodes reach every node.
