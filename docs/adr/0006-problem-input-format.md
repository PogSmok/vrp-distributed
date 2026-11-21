# 0006. Problem input format: custom JSON

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** 0008, 0013, 0015

## Context

Users submit VRP problems through the client application. The format defines
what a problem looks like: depot, customers with locations and demands, and
vehicle count and capacity. The main purpose of the project is to measure how
the solver scales with the number of GPUs, so problems must be easy to
generate, reproduce and inspect. Scaling is measured against our own
single-GPU runs. That alone cannot show whether the solver is good: a weak
solver can also scale well, so solution quality needs an external reference.

## Decision drivers

- Human-readable, easy-to-debug problem files.
- Reproducible generation of benchmark problems.
- Node IDs are only valid within one graph version (0015), so the input must
  not depend on internal IDs.
- Solver quality must be comparable with published results.

## Decision

Problems are described in a custom JSON format with an explicit
`schema_version` field. Customer locations are given as latitude/longitude;
they are snapped to the nearest node of the job's pinned graph version in the
PREPARE phase (0013). A customer further than a configured maximum snapping
distance from any node makes the job fail with an explicit error listing
those customers. The client validates the JSON and converts it into the
Protobuf problem message of the gRPC contract (0008). A generator script
produces problems from a fixed random seed, a region and a number of
customers.

Solver quality is validated separately from the job pipeline: CVRPLIB
instances are given to the solver directly as explicit distance matrices,
and the results are compared with the best known solutions.

## Alternatives considered

### Option A: Standard benchmark format (TSPLIB-like, for example CVRPLIB) as the job input
- Pros: allows comparing solution quality with published results.
- Cons: the instances use planar coordinates and Euclidean distances, so they
  cannot be placed on a road graph.
- Why rejected as the job input: jobs must run on road-network distances.
  CVRPLIB is still used for solver validation, where the solver only needs a
  distance matrix (see Decision).

### Option B: Protobuf only, no file format
- Pros: one representation everywhere.
- Cons: binary files cannot be read or edited by hand.
- Why rejected: human-readable files are worth the small conversion step.

## Consequences

**What we gain:**
- Easy debugging and reproducible benchmarks through seeded generation.
- Solver quality is checked against published results, independently of
  the road graph.

**What we pay for it:**
- Published results validate the solver only, not the road-network distances.
- A JSON schema and validator to maintain; the format must be versioned.
- A CVRPLIB reader for the solver's validation harness.

**Risks / to revisit later:**
- JSON is verbose for very large problems; revisit if file size or parsing
  time becomes an issue.

## Confirmation

Generate the same problem twice with the same seed and verify that the files
are identical. Submit a generated problem and check that the PREPARE phase
produces the same node assignment on every run. Submit a customer far from
any road and check that the job fails with an explicit error. Run a set of
CVRPLIB instances and report the gap to the best known solutions.
