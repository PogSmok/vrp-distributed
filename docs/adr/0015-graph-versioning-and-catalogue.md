# 0015. Graph versioning and catalogue

- **Status:** Accepted; upload and registration refined by 0031
- **Date:** 2026-11-21
- **Related:** 0005, 0006, 0007, 0016, 0020, 0027

## Context

Node IDs change with every import, so a job, its matrix and its result mean
something only together with the graph they used. The orchestrator must also
know which graphs exist.

## Decision

Each import produces an immutable graph version, whose ID comes from the
region, the extract date and a checksum. The preprocessor uploads the graph
and a manifest, then registers the version with an admin token. The manifest
holds the node and edge counts, the edge weights and speed table version
(0020), the checksum, the tool version, and the mapping from node IDs to OSM
IDs. Jobs, matrices and results record their version, and a version is kept
while any result refers to it.

## Alternatives considered

- **OSM node IDs everywhere:** compact formats need dense internal IDs.
- **Only the latest graph:** older results become uninterpretable.
- **Discovering versions by listing storage:** partial uploads would be
  visible.

## Consequences

- Gain: reproducible jobs, safe caches, and a defined moment when a graph
  becomes available.
- Cost: a catalogue to maintain; storage grows with each version.
- Revisit: a retention policy for versions no result refers to.

## Confirmation

Jobs run on two versions refer to the right one. An interrupted upload is
never offered to workers.
