# 0027. Graph file format: custom little-endian binary with a JSON manifest

- **Status:** Accepted
- **Date:** 2026-11-22
- **Related:** 0005, 0006, 0007, 0011, 0015, 0016, 0020, 0023, 0025

## Context

The Python preprocessor writes graphs and the C++ vrp-core reads them, and
graphs of a few hundred MB travel through the object store to every worker
(0007). vrp-core keeps its dependencies minimal (0023), and the GPU kernels
want the graph in compressed sparse rows. A simplified graph (0011) must
still let routes be drawn along the roads (0016, 0025).

## Decision

- **Files.** A graph version is up to four files: `graph.bin` (compressed
  sparse rows, both weights, positions) for the solvers; `edge_geometry.bin`
  (each edge's shape points) for route polylines; `osm_node_ids.bin` for
  people; and a `manifest.json` with counts, checksums, source,
  simplification cap, speed table and generator. Keeping geometry and OSM
  IDs out of `graph.bin` keeps them off the GPU and out of most downloads.
  This refines 0015, which put the OSM mapping in the manifest.
- **Container.** The binary files share a header and a section table of
  tagged, 8-byte-aligned arrays. Files can be memory-mapped, and readers
  skip unknown sections, so new sections need no new format version.
- **Simplification.** Shape-only nodes are merged into longer edges, capped
  at a maximum length so that customers still snap close to the road
  (0006).
- **Reproducibility.** A canonical order makes the same input and generator
  version produce identical bytes.

The layout, the reader's checks and golden files are in
`docs/formats/graph.md`.

## Alternatives considered

- **NumPy `.npz`:** trivial in Python, but the C++ side needs a ZIP and
  `.npy` parser, and compressed members cannot be memory-mapped.
- **Apache Arrow or Parquet:** robust and self-describing, but a heavy C++
  dependency for vrp-core.
- **HDF5:** also a heavy dependency, and awkward to build on Windows.
- **FlatBuffers:** needs a schema compiler in both builds for a handful of
  flat arrays; Protobuf cannot hold messages over 2 GB.
- **No simplification:** simpler, and polylines come from nodes alone, but
  the graph is several times larger on the GPU, where the oldest card
  (GTX 1060) has the least room.

## Consequences

- Gain: a small reader and writer on each side with no dependencies; arrays
  that go straight to the GPU; a smaller graph; sections that can be added
  without re-importing.
- Cost: we maintain the spec, both implementations and the golden files; no
  general tools can inspect the files.
- Revisit: compression (for example zstd), if downloads become slow; the
  simplification cap, after measuring snapping error on a real region.

## Confirmation

The Python writer reproduces the golden files byte for byte, and the C++
reader loads them with every value as the spec lists, both in CI. The
reader rejects a truncated file, a wrong magic number, a missing required
section and an out-of-range target. Writing the same input twice gives the
same SHA-256.
