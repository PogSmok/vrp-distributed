# 0006. Problem input format: custom JSON

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** 0008, 0012, 0013, 0015, 0020

## Context

Problems (a depot, customers with locations and demands, a vehicle count
and capacity) must be easy to generate, reproduce and inspect, and must not
depend on graph node IDs (0015). Scaling measured against our own runs says
nothing about quality, so quality needs an external reference.

## Decision

Problems are JSON files with a `schema_version`, latitude and longitude for
each stop, and the edge cost to optimise (`length` or `time`, 0020). The
client validates a problem and converts it to Protobuf. The orchestrator
rejects problems that cannot be feasible: a customer's demand above the
vehicle capacity, or total demand above the fleet's capacity. PREPARE snaps
stops to the job's graph, and a customer too far from any road fails the job
with a list of such customers. A generator produces problems from a seed, a
region and a customer count. Solver quality is validated separately, on
CVRPLIB instances given as explicit matrices.

## Alternatives considered

- **CVRPLIB as the job input:** its planar instances cannot be placed on
  roads; it is used for validation only.
- **Protobuf files only:** not human-readable.

## Consequences

- Gain: easy debugging, reproducible benchmarks, and quality comparable with
  published results.
- Cost: a schema and validator to version; CVRPLIB validates the solver, not
  the road distances.

## Confirmation

The same seed gives identical files, and the same problem snaps the same
way on every run. A customer far from any road, and a problem with too much
demand, are rejected with explicit errors. CVRPLIB gaps are reported.
