# 0015. Graph versioning and catalogue

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** 0005, 0006, 0007, 0016

## Context

Graph node IDs are internal and change on every re-import. A job, its
distance matrix and its result are only meaningful together with the graph
they were computed on. The orchestrator also needs to know which graphs exist
before it can assign them.

## Decision drivers

- Reproducible results: a result can always be tied to its graph.
- The orchestrator learns about graphs without scanning storage.
- Workers can verify the files they cache.

## Decision

Each preprocessing run produces an immutable graph version with an ID derived
from the region, the source extract date and a content checksum. The
preprocessor uploads the graph file and a manifest (region, source extract
date, node and edge counts, checksum, tool version, mapping from internal
node IDs to OSM IDs) to the object store, then registers the version with the
orchestrator using an admin token. The orchestrator keeps a catalogue of
versions in the state database. Every job records the `graph_version` it was
submitted against, and distance matrices and results reference it. Old
versions are kept while any stored result refers to them.

## Alternatives considered

### Option A: Use OSM node IDs everywhere
- Pros: stable across imports, so no versioning is needed.
- Cons: simplified graphs drop nodes, and compact array formats need dense
  internal IDs anyway.
- Why rejected: internal IDs are required; OSM IDs are kept only as a mapping.

### Option B: Keep only the latest graph, with no versions
- Pros: simplest.
- Cons: results become uninterpretable after the next import, and benchmarks
  cannot be repeated.
- Why rejected: breaks reproducibility.

### Option C: The orchestrator discovers versions by listing the object store
- Pros: no registration step.
- Cons: partial uploads are visible, and there is no authoritative status.
- Why rejected: explicit registration after a complete upload is safer.

## Consequences

**What we gain:**
- Reproducible jobs, safe caches, and a defined moment when a new graph
  becomes available.

**What we pay for it:**
- A catalogue table and a registration call to maintain; storage grows with
  retained versions.

**Risks / to revisit later:**
- Define a retention policy for versions that no result refers to.

## Confirmation

Run the same job against two graph versions and verify that the results
reference the right version. Interrupt an upload and verify that the
unfinished version is never offered to workers.
