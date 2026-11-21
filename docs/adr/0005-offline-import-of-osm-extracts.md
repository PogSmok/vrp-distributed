# 0005. Offline import of regional OSM extracts

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** 0007, 0011, 0015

## Context

The solver needs a road network. OpenStreetMap is the data source. The graph
must be preprocessed (filtered to drivable roads, converted to a compact
array format) before it can be loaded into GPU memory. Raw OSM road graphs
are not strongly connected: one-way dead ends, ferries and private areas
create nodes that cannot be reached or left. Benchmarks should be
reproducible.

## Decision drivers

- Reproducible benchmarks on fixed data snapshots.
- No runtime dependency on external infrastructure.
- The preprocessed regional graph should fit on a single GPU (0011).

## Decision

We import regional OSM extracts (`.pbf`) offline with a separate preprocessing
tool written in Python using pyrosm. The running system never queries OSM.
The preprocessor keeps only the largest strongly connected component of the
drivable graph, so every node can reach every other node.

Implementation note: pyrosm is the initial reader because it reads `.pbf`
files directly. The output format does not depend on it, so it can be
replaced (for example by pyosmium) if the graph no longer fits in memory.
Verify pyrosm's maintenance status and compatibility with the chosen Python
version before building on it.

## Alternatives considered

### Option A: Query a live API (for example Overpass) at run time
- Pros: always current data, no files to manage.
- Cons: public APIs are not meant for bulk downloads and impose limits; adds
  latency and an external runtime dependency; results change between runs.
- Why rejected: unsuitable for bulk data and for reproducible benchmarks.

### Option B: Import the whole planet
- Pros: global coverage.
- Cons: tens of gigabytes to download, and the graph is far larger than a
  single GPU can hold.
- Why rejected: a regional extract is enough to demonstrate the system.

### Option C: Reuse the data of a routing engine (OSRM, Valhalla)
- Pros: ready-made, optimised graphs.
- Cons: engine-specific formats that our GPU kernels cannot use without
  conversion; hides the part of the work we want to show.
- Why rejected: no benefit for our own kernels.

## Consequences

**What we gain:**
- Reproducible benchmarks; no dependency on OSM infrastructure at run time.
- Graph size is a controllable benchmark axis (city, region, country).

**What we pay for it:**
- Data can be stale until the next import; an administrator must run the
  preprocessor.
- Roads outside the largest strongly connected component are dropped, so a
  few customers may snap to a node further away.

**Risks / to revisit later:**
- OSM data is licensed under ODbL, so attribution is required.
- OSM publishes replication diffs; applying them could be future work.
- The edge cost (road length or travel time) is not decided yet; see the
  planned ADRs in the README.

## Confirmation

Preprocess the target region, measure the resulting compact graph size, and
check that it fits on the smallest GPU in use (see 0011). Run shortest paths
from a sample of random nodes and check that every node is reached.
