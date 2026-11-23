# Result file format, schema version 1

A result is a UTF-8 JSON file that FINALIZE uploads to the object store
(ADR 0016, 0028): the routes along the roads, and what produced them.

```json
{
  "schema_version": 1,
  "graph_version": "golden-20261122-cd9e8b23",
  "edge_cost": "length",
  "cost": 533,
  "stop_nodes": [0, 1, 2],
  "routes": [
    {"stops": [1], "cost": 222, "polyline": [[52.0000000, 21.0000000], [52.0005000, 21.0001000], [52.0010000, 21.0000000], [52.0000000, 21.0000000]]},
    {"stops": [2], "cost": 311, "polyline": [[52.0000000, 21.0000000], [52.0000000, 21.0010000], [52.0004000, 21.0009000], [52.0008000, 21.0006000], [52.0010000, 21.0000000], [52.0000000, 21.0000000]]}
  ],
  "parameters": {"seconds": 1.5, "seed": 7},
  "workers": [
    {"name": "lab-pc-1", "build": "0.1.0", "gpu": "NVIDIA GeForce GTX 1060 6GB"},
    {"name": "lab-pc-2", "build": "0.1.0", "gpu": "NVIDIA GeForce RTX 5050 Laptop GPU"}
  ],
  "islands": [
    {"worker": "lab-pc-1", "convergence": [[0.25, 533]]},
    {"worker": "lab-pc-2", "convergence": [[0.5, 533]]}
  ]
}
```

| Field            | Type    | Meaning                                                                |
|------------------|---------|------------------------------------------------------------------------|
| `schema_version` | integer | `1`                                                                    |
| `graph_version`  | string  | The graph version the routes were computed on (ADR 0015)               |
| `edge_cost`      | string  | `length` (whole metres) or `time` (tenths of a second), ADR 0020       |
| `cost`           | integer | Total cost: the sum of the routes' costs                               |
| `stop_nodes`     | array   | The node of `graph_version` that each stop snapped to, the depot first |
| `routes`         | array   | One object per vehicle used; see below                                 |
| `parameters`     | object  | Every solver parameter by name, with the value used, defaults included |
| `workers`        | array   | Every worker involved: its `name` (ADR 0009), `build` and `gpu`        |
| `islands`        | array   | One object per island, in island order; see below                      |

Stops are numbered as in the problem file: the depot is stop 0 and customer
`i` of the problem is stop `i + 1` (`docs/formats/problem.md`).

**Routes.** `stops` lists a vehicle's customers in visiting order; the depot
is implicit at both ends. `cost` is the route's cost in the units of
`edge_cost`. `polyline` runs along the roads from the depot through every
stop and back, as `[latitude, longitude]` pairs in degrees (WGS 84) with 7
decimals, the graph's precision: road shape points and graph nodes, joined by
straight lines.

**Islands.** `worker` names the worker that ran the island. `convergence`
lists `[seconds, cost]` pairs, one each time the island's best feasible cost
improved, with `seconds` counted from the start of the island's solve task,
which includes downloading the matrix.

Costs are integers, at most 2⁶⁴ − 1 (ADR 0020). Readers ignore fields they do
not know, so later writers can add fields within the same schema version;
changing the meaning of a field increments `schema_version`.

## Golden file

`result-golden-v1.json` is the example above, routed on the golden graph of
`docs/formats/graph.md` for a problem with two vehicles of capacity 1, so
that each customer gets its own vehicle; its parameters are abbreviated.
Route 0 leaves along edge 0 and its shape point; route 1 returns from node 2
through node 1, along edge 3 and its two shape points. vrp-core's writer reproduces it byte for byte, so the
spec itself, not the implementation, defines what is correct.
