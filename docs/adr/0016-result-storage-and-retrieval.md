# 0016. Result storage and retrieval

- **Status:** Accepted; lifecycle rule replaced by 0029
- **Date:** 2026-11-21
- **Related:** 0009, 0013, 0014, 0015, 0022, 0025

## Context

Results (routes as road polylines, and the convergence history) must
survive client disconnects and stay interpretable, without bloating the
database.

## Decision

FINALIZE builds a result artifact and uploads it to the object store. It
holds the routes as node IDs and polylines, the cost and edge cost, each
island's convergence history, the graph version, the solver parameters, and
the build and GPU of every worker involved. The database keeps only
metadata. While a job runs, clients poll for progress (0022); afterwards
they get a presigned download URL and a checksum. Results and matrices are
kept for 30 days. Uploads from failed attempts are deleted when their task
completes. A lifecycle rule removes anything left over, such as objects from
a crash, or a late upload through a URL that cannot be revoked.

## Alternatives considered

- **Results in the database:** large polylines bloat it.
- **Returning results only to a connected client:** lost if the client is
  away.

## Consequences

- Gain: a reconnecting client can fetch its result; the database stays
  small.
- Cost: a cleanup process and an artifact format to version; matrices use
  storage for 30 days.

## Confirmation

Reconnect after a job completes and download its result. Check that expired
results and matrices are gone, and that an expired attempt's objects are
deleted.
