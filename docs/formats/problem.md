# Problem file format, schema version 1

A problem is a UTF-8 JSON file (ADR 0006): a depot, customers with positions
and demands, a fleet of identical vehicles, and the edge cost to optimise.
Positions are latitude and longitude, never graph node IDs, so a problem
stays valid across graph versions (ADR 0015); PREPARE snaps them to nodes.

```json
{
  "schema_version": 1,
  "edge_cost": "time",
  "vehicle_count": 2,
  "vehicle_capacity": 10,
  "depot": {"latitude": 52.2297, "longitude": 21.0122},
  "customers": [
    {"latitude": 52.2310, "longitude": 21.0201, "demand": 4},
    {"latitude": 52.2254, "longitude": 21.0035, "demand": 7}
  ]
}
```

| Field              | Type    | Meaning                                                                  |
|--------------------|---------|--------------------------------------------------------------------------|
| `schema_version`   | integer | `1`                                                                      |
| `edge_cost`        | string  | `length` (whole metres) or `time` (tenths of a second), ADR 0020         |
| `vehicle_count`    | integer | Number of vehicles                                                       |
| `vehicle_capacity` | integer | Capacity of each vehicle, in the units of `demand`                       |
| `depot`            | object  | `latitude` and `longitude` in degrees (WGS 84)                           |
| `customers`        | array   | One object per customer: `latitude`, `longitude` and integer `demand`    |

Every field is required. Integers are at most 4,294,967,295 (2³² − 1).
Readers ignore fields they do not know, so later writers can add fields
within the same schema version.

**Stops.** The depot is stop 0 and customer `i` of the array is stop `i + 1`.
The distance matrix and the solution's routes use these numbers (ADR 0011).

**Checks.** A reader rejects a file that breaks the rules above. A problem
that cannot be feasible is rejected too (ADR 0006): no vehicles, a
customer's demand above the vehicle capacity, or a total demand above the
fleet's. PREPARE then fails the job, listing the customers, if any stop is
too far from every road.
