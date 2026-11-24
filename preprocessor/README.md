# vrp-preprocessor

Turns OpenStreetMap extracts into versioned road graphs in the format of
`docs/formats/graph.md` (ADR 0005, 0015, 0027), and writes synthetic graphs
for testing. `vrp-preprocessor problem` writes a random problem on a graph
version, in the format of `docs/formats/problem.md` (ADR 0006). It is a
member of the Python workspace defined by the root `pyproject.toml`.

## Setup

Install [uv](https://docs.astral.sh/uv/), then, from the repository root:

```sh
uv sync --all-packages # creates .venv from uv.lock
uv run vrp-preprocessor --version
```

## Commands

```sh
vrp-preprocessor import --extract mazowieckie-261121.osm.pbf --region mazowieckie --output mazowieckie
vrp-preprocessor synthetic grid --width 2000 --height 2000 --output grid
vrp-preprocessor problem --graph grid --customers 5000 --output problem.json
```

`import` turns a regional extract, such as one from Geofabrik, into a graph
version:

1. pyrosm reads the drivable roads, with its `driving+service` filter, as
   segments between consecutive nodes.
2. Each segment becomes an edge in each direction it can be driven:
   `oneway` decides, motorways and roundabouts are one-way unless tagged
   otherwise, and ways whose direction changes with the time of day
   (`oneway=reversible`) are left out. Its length is the great-circle
   distance between its nodes, and its time uses `maxspeed` if that starts
   with a number, or else its road class's default from `osm.SPEED_TABLE`
   (ADR 0020), which the manifest records. The defaults are Poland's
   general speed limits, as `maxspeed` is a limit too, so times are
   optimistic. Road classes not in the table are not imported.
3. Only the largest strongly connected component is kept, so that every
   node can reach every other (ADR 0005).
4. Nodes that only trace a road's shape are merged into edges of at most
   `--max-edge-length-m`, 250 by default (`docs/formats/graph.md`).
5. Nodes are ordered along a Hilbert curve, so that nodes near each other
   on the map are near each other in memory.

The admin tool uploads and registers a graph version's directory
(`deploy/README.md`); the preprocessor itself holds no credentials.

The same extract and preprocessor version give the same bytes. The graph
version ID's date comes from the extract's header, its replication
timestamp; `--extract-date` gives it for an extract whose header has none,
and must agree with the header otherwise. pyrosm bounds its memory by
spilling to the temporary folder, and the import clears what it left there.

Graphs imported from OpenStreetMap are © OpenStreetMap contributors, under
the ODbL: anything that shows them, such as the client's maps, must say so.
