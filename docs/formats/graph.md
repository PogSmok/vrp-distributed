# Graph file format, version 1

This is the contract between the graph preprocessor, which writes graphs, and
vrp-core, which reads them (ADR 0027). A graph version (ADR 0015) is stored
in the object store as up to four files:

```
graphs/<graph_version>/manifest.json       what the graph is, and checksums
graphs/<graph_version>/graph.bin           what solvers need; workers download it
graphs/<graph_version>/edge_geometry.bin   road shape of each edge; for route polylines
graphs/<graph_version>/osm_node_ids.bin    node IDs to OSM IDs; for people, not solvers
```

Only `graph.bin` goes to the GPU. FINALIZE also reads `edge_geometry.bin`, to
draw routes along the roads (ADR 0016, 0025). Nothing in the pipeline reads
`osm_node_ids.bin`.

## Simplification

The preprocessor merges chains of nodes that only trace a road's shape
(nodes with one road in and one road out) into single edges, and stores the
removed points in `edge_geometry.bin`. This makes the graph several times
smaller. It never makes an edge longer than `max_edge_length_m` from the
manifest, because customers snap to nodes (ADR 0006): with a 250 m cap,
every point of a road is within 125 m of a node. An edge's length and time
are the sums along its whole shape, not the straight line between its ends.

## Binary container

`graph.bin`, `edge_geometry.bin` and `osm_node_ids.bin` share one layout: a
header, a section table, then the sections.

- All integers are little-endian. Readers on a big-endian host refuse the
  file.
- Readers compute byte offsets and sizes in 64-bit arithmetic: `4 × E`
  overflows 32 bits for large graphs. Even 64 bits can overflow for a corrupt
  `count`, so a reader checks that a section fits by comparing
  `count ≤ (file_size − offset) / element_size` (after checking
  `offset ≤ file_size`), never by computing `offset + count × element_size`.
- Every section starts at a byte offset that is a multiple of 8, padded with
  zero bytes, so a reader can map the file into memory and use the arrays
  directly. Sections appear in the order of the table, without gaps beyond
  that padding, and the file ends at the end of the last section, rounded up
  to a multiple of 8.
- Node, edge and shape-point counts are at most 4,294,967,294 (2³² − 2), so
  every index fits in 32 bits and 2³² − 1 stays free as a "no node" marker.

### Header (32 bytes)

| Offset | Size | Type   | Field            | Value                                                          |
|--------|------|--------|------------------|----------------------------------------------------------------|
| 0      | 8    | bytes  | `magic`          | ASCII, identifies the file: `VRPGRAPH`, `VRPSHAPE`, `VRPOSMID` |
| 8      | 4    | uint32 | `format_version` | `1`                                                            |
| 12     | 4    | uint32 | `section_count`  | Number of entries in the section table                         |
| 16     | 4    | uint32 | `node_count`     | N, the same in every file of a graph version                   |
| 20     | 4    | uint32 | `edge_count`     | E, the same in every file of a graph version                   |
| 24     | 4    | uint32 | `flags`          | `0`; no flags are defined in version 1                         |
| 28     | 4    | uint32 | reserved         | `0`                                                            |

### Section table (24 bytes per entry, starting at offset 32)

| Offset | Size | Type   | Field          | Value                                          |
|--------|------|--------|----------------|------------------------------------------------|
| 0      | 4    | bytes  | `tag`          | Four ASCII characters naming the section       |
| 4      | 4    | uint32 | `element_type` | `1` uint32, `2` int32, `3` int64               |
| 8      | 8    | uint64 | `offset`       | Byte offset of the section from the file start |
| 16     | 8    | uint64 | `count`        | Number of elements                             |

Readers ignore sections whose tag they do not know. A writer can therefore
add a section without a new format version, as long as the existing
sections keep their meaning.

## `graph.bin`

Magic `VRPGRAPH`. Required sections:

| Tag    | Type   | Count | Meaning                                                                      |
|--------|--------|-------|------------------------------------------------------------------------------|
| `OFFS` | uint32 | N + 1 | Outgoing edges of node v are `[OFFS[v], OFFS[v + 1])`                        |
| `TARG` | uint32 | E     | Head node of each edge                                                       |
| `LENG` | uint32 | E     | Length in whole metres along the edge's shape, rounded to nearest (ADR 0020) |
| `TIME` | uint32 | E     | Travel time in tenths of a second, rounded to nearest (ADR 0020)             |
| `LATS` | int32  | N     | Latitude in units of 10⁻⁷ degrees (OSM's own precision)                      |
| `LONS` | int32  | N     | Longitude in units of 10⁻⁷ degrees                                           |

This is the compressed sparse row form that vrp-core and the GPU kernels
use, so loading needs no conversion. Incoming edges are not stored; readers
build them. Weights of 0 are allowed: rounding can produce them, and the
searches handle them.

### Canonical order

For the same input and the same generator version, a writer produces
byte-identical files, so checksums and graph version IDs are reproducible
(ADR 0005, 0015). A new generator version may change the bytes, and so the
version ID; that is intended.

- **Nodes:** the writer chooses the order, deterministically. The
  preprocessor orders nodes for memory locality, for example along a
  space-filling curve. Readers must not rely on any order.
- **Edges:** grouped by tail node, as the offsets require. Within a node,
  sorted by target, then length, then time.
- **Sections:** a version 1 writer writes exactly the required sections, in
  the order of the table above. This rule binds writers only: readers still
  accept and skip sections they do not know.
- **Parallel edges** between the same two nodes are allowed. **Self-loops**
  are not.

### Size

For 5 million nodes and 12.5 million edges, `graph.bin` is about 210 MB:
20 MB of offsets, 50 MB each for targets, lengths and times, and 40 MB of
positions. A worker keeps the offsets, the targets and one weight array on
the GPU: about 120 MB.

## `edge_geometry.bin`

Magic `VRPSHAPE`. The points between each edge's two end nodes, in the
direction of travel. The end nodes themselves are not repeated. Required
sections:

| Tag    | Type   | Count | Meaning                                                    |
|--------|--------|-------|------------------------------------------------------------|
| `SOFF` | uint32 | E + 1 | Shape points of edge e are `[SOFF[e], SOFF[e + 1])`        |
| `SLAT` | int32  | P     | Latitude of each shape point, in 10⁻⁷ degrees; P = SOFF[E] |
| `SLON` | int32  | P     | Longitude of each shape point, in 10⁻⁷ degrees             |

Edges are numbered as in `graph.bin`. An unsimplified graph, such as a
synthetic one, omits this file, and every edge is a straight line.

## `osm_node_ids.bin`

Magic `VRPOSMID`. One required section:

| Tag    | Type  | Count | Meaning                      |
|--------|-------|-------|------------------------------|
| `OSMN` | int64 | N     | OSM ID of node v, at index v |

Synthetic graphs, which have no OSM source, omit this file.

## What a reader must check

A reader fails, without crashing or reading out of bounds, unless all of
these hold:

1. The file is at least 32 bytes, `magic` is the expected one,
   `format_version` is supported, `flags` is 0, and the section table fits
   in the file.
2. `node_count` and `edge_count` are at most 2³² − 2, and equal the counts
   in the manifest.
3. Each required tag appears exactly once, with the right element type and
   count. Every section is 8-byte aligned, follows the previous one, and the
   file size is exactly the end of the last section rounded up to 8.
4. `OFFS[0]` is 0, the offsets never decrease, and `OFFS[N]` equals E. The
   same holds for `SOFF`, ending at P.
5. Every target is less than N, and no edge's target equals its tail.
6. Every latitude is within ±900,000,000 and every longitude within
   ±1,800,000,000.

Readers do not check the canonical order, that lengths match the geometry,
or strong connectivity. Those are the writer's responsibility; strong
connectivity is checked in the preprocessor's tests (ADR 0005), not at load
time.

**Integrity.** A worker verifies `graph.bin` against the SHA-256 that comes
with its assignment, which the orchestrator takes from the catalogue
(ADR 0007, 0015). It does not trust a manifest stored next to the file,
because a corrupt cache could include the manifest.

## `manifest.json`

UTF-8 JSON. Readers ignore fields they do not know, so later writers can add
fields within the same format version. Every `path` is a plain file name,
relative to the manifest's own directory: never absolute, never an
object-store key, and never containing `/`, `\` or `..`.

| Field                      | Type          | Required | Meaning                                                                                           |
|----------------------------|---------------|----------|---------------------------------------------------------------------------------------------------|
| `format_version`           | integer       | yes      | `1`; must equal the version in the binary files                                                   |
| `graph_version`            | string        | yes      | The graph version ID; see below                                                                   |
| `region`                   | string        | yes      | Lower-case slug, `[a-z0-9-]+`, for example `mazowieckie`                                          |
| `node_count`, `edge_count` | integer       | yes      | Must equal the binary headers                                                                     |
| `files.graph`              | object        | yes      | `path`, `size_bytes`, `sha256` (lower-case hex) of `graph.bin`                                    |
| `files.edge_geometry`      | object        | no       | The same, for `edge_geometry.bin`; absent for unsimplified graphs                                 |
| `files.osm_node_ids`       | object        | no       | The same, for `osm_node_ids.bin`; absent for synthetic graphs                                     |
| `source`                   | object / null | yes      | `extract` (file name), `extract_date` (`YYYY-MM-DD`), `sha256`; `null` for synthetic graphs       |
| `simplification`           | object / null | yes      | `max_edge_length_m`, the cap described above; `null` for unsimplified graphs                      |
| `speed_table`              | object        | yes      | `version` and `speeds_kmh`, the default speed of each road class used for travel times (ADR 0020) |
| `generator`                | object        | yes      | `name` and `version` of the tool that wrote the graph, and `osm_library` if it used one           |
| `created_at`               | string        | yes      | UTC time in ISO 8601, for example `2026-11-22T10:00:00Z`; the only field that is not reproducible |

### Graph version ID

`<region>-<YYYYMMDD>-<hash>`, where the date is the extract date and `<hash>`
is the first 8 hex characters of `graph.bin`'s SHA-256: for example
`mazowieckie-20261101-3f2a9c1e`. Synthetic graphs have no extract date and
use `<region>-<hash>`, for example `synthetic-grid-2000x2000-9b04e7d2`.

## Changing the format

Adding a section, or a manifest field, needs no new version. Changing the
layout of the header or table, or the meaning of an existing section or
field, increments `format_version` in every file. Readers reject versions
they do not support, and stored graphs are re-imported rather than
converted.

## Golden files

`docs/formats/golden-v1/` holds a complete three-node graph version: all
four files. Both the Python writer and the C++ reader are tested against
these bytes, so the spec itself, not either implementation, defines what is
correct. Any change to the golden files is a change to this spec.

The graph has nodes 0, 1, 2 and four edges; edge 0 has one shape point and
edge 3 has two. The values are chosen to be easy to read in a hex dump, not
to be geographically consistent.

| Section | Values                                |
|---------|---------------------------------------|
| `OFFS`  | 0, 2, 3, 4                            |
| `TARG`  | 1, 2, 0, 1 (edges 0→1, 0→2, 1→0, 2→1) |
| `LENG`  | 111, 68, 111, 132                     |
| `TIME`  | 133, 82, 133, 159                     |
| `LATS`  | 520000000, 520010000, 520000000       |
| `LONS`  | 210000000, 210000000, 210010000       |
| `SOFF`  | 0, 1, 1, 1, 3                         |
| `SLAT`  | 520005000, 520004000, 520008000       |
| `SLON`  | 210001000, 210009000, 210006000       |
| `OSMN`  | 2540118411, 2540118412, 314159265     |

`graph.bin` in full (272 bytes):

```text
00000000  56 52 50 47 52 41 50 48 01 00 00 00 06 00 00 00  VRPGRAPH........
00000010  03 00 00 00 04 00 00 00 00 00 00 00 00 00 00 00  ................
00000020  4f 46 46 53 01 00 00 00 b0 00 00 00 00 00 00 00  OFFS............
00000030  04 00 00 00 00 00 00 00 54 41 52 47 01 00 00 00  ........TARG....
00000040  c0 00 00 00 00 00 00 00 04 00 00 00 00 00 00 00  ................
00000050  4c 45 4e 47 01 00 00 00 d0 00 00 00 00 00 00 00  LENG............
00000060  04 00 00 00 00 00 00 00 54 49 4d 45 01 00 00 00  ........TIME....
00000070  e0 00 00 00 00 00 00 00 04 00 00 00 00 00 00 00  ................
00000080  4c 41 54 53 02 00 00 00 f0 00 00 00 00 00 00 00  LATS............
00000090  03 00 00 00 00 00 00 00 4c 4f 4e 53 02 00 00 00  ........LONS....
000000a0  00 01 00 00 00 00 00 00 03 00 00 00 00 00 00 00  ................
000000b0  00 00 00 00 02 00 00 00 03 00 00 00 04 00 00 00  ................
000000c0  01 00 00 00 02 00 00 00 00 00 00 00 01 00 00 00  ................
000000d0  6f 00 00 00 44 00 00 00 6f 00 00 00 84 00 00 00  o...D...o.......
000000e0  85 00 00 00 52 00 00 00 85 00 00 00 9f 00 00 00  ....R...........
000000f0  00 92 fe 1e 10 b9 fe 1e 00 92 fe 1e 00 00 00 00  ................
00000100  80 58 84 0c 80 58 84 0c 90 7f 84 0c 00 00 00 00  .X...X..........
```

The header is the first 32 bytes. The section table, 6 entries of 24 bytes,
runs to offset `0xb0`, where `OFFS` begins. `LATS` ends at `0xfc` and is
padded to `0x100`.
