# Architecture Decision Records

An ADR records **one** architectural decision and why it was made. The
diagrams in `docs/architecture/` show what the system looks like, and the
numbers ADRs refer to live in `docs/quality-attributes.md`.

## When to write one

Write an ADR for a decision that is hard to reverse, has realistic
alternatives, or affects more than one component. Record smaller choices in
the README of the component they concern.

## How to add one

1. Copy `0000-template.md` to `NNNN-short-title.md`, using the next free
   number.
2. Fill in every section briefly: most sections take one to three
   sentences. List only real alternatives, and say in *Confirmation* how the
   decision will be checked.
3. Use `Proposed` while the decision is open and `Accepted` once it is made.
   Accepted means decided, not proven; if the confirmation fails, a new ADR
   supersedes this one.
4. Add a row to the index, and commit the ADR with the code or diagram
   change it belongs to (including `docs/architecture/workspace.dsl`).

## How to change a decision

Once an ADR is merged as `Accepted`, do not rewrite it; before merging it is
a draft. Write a new ADR that references the old one, set the old one to
`Superseded by NNNN`, and update the index. Typos and broken links can be
fixed in place.

## Conventions

One file per decision; titles state the decision, not the question; numbers
are never reused; present tense.

## Index

| #    | Title                                                                                                                      | Status                         |
|------|----------------------------------------------------------------------------------------------------------------------------|--------------------------------|
| 0001 | [Job and state store: PostgreSQL](0001-job-and-state-store-postgresql.md)                                                  | Accepted                       |
| 0002 | [Job assignment over worker-initiated gRPC streams](0002-job-assignment-over-worker-initiated-streams.md)                  | Accepted                       |
| 0003 | [Orchestrator as the single owner of job state](0003-orchestrator-owns-job-state.md)                                       | Accepted                       |
| 0004 | [Island migration via orchestrator-held migrant pool](0004-island-migration-via-migrant-pool.md)                           | Accepted; see 0034             |
| 0005 | [Offline import of regional OSM extracts](0005-offline-import-of-osm-extracts.md)                                          | Accepted                       |
| 0006 | [Problem input format: custom JSON](0006-problem-input-format.md)                                                          | Accepted                       |
| 0007 | [Graph distribution: object store with local cache](0007-graph-distribution-object-store-with-local-cache.md)              | Accepted; see 0029, 0031       |
| 0008 | [Control-plane protocol: gRPC](0008-control-plane-protocol-grpc.md)                                                        | Accepted                       |
| 0009 | [Authentication of clients and workers; scoped storage access](0009-authentication-and-scoped-storage-access.md)           | Accepted; see 0031             |
| 0010 | [Monitoring with Prometheus and Grafana; logs collected by the orchestrator](0010-monitoring-as-external-systems.md)       | Accepted                       |
| 0011 | [Work split across GPUs: full graph per worker, two-phase pipeline](0011-work-split-across-gpus.md)                        | Accepted                       |
| 0012 | [No separate API layer; orchestrator exposes the client interface](0012-no-separate-api-layer.md)                          | Accepted                       |
| 0013 | [Job and task model with group scheduling](0013-job-and-task-model-with-group-scheduling.md)                               | Accepted                       |
| 0014 | [Failure and recovery model](0014-failure-and-recovery-model.md)                                                           | Accepted; bounded by 0032      |
| 0015 | [Graph versioning and catalogue](0015-graph-versioning-and-catalogue.md)                                                   | Accepted; see 0031             |
| 0016 | [Result storage and retrieval](0016-result-storage-and-retrieval.md)                                                       | Accepted; see 0029             |
| 0017 | [GPU distance algorithm: batched frontier Bellman-Ford on the plain graph](0017-gpu-distance-algorithm.md)                 | Accepted; see 0033             |
| 0018 | [Solver algorithm: memetic search on giant tours, local search on the GPU](0018-solver-algorithm.md)                       | Accepted; see 0034             |
| 0019 | [Deployment: third-party services in Docker Compose, our programs native](0019-deployment-topology-and-build-platforms.md) | Accepted; see 0029, 0031, 0035 |
| 0020 | [Edge cost: road length and travel time stored, chosen per job](0020-edge-cost-length-and-travel-time.md)                  | Accepted                       |
| 0021 | [Orchestrator core: single-threaded, event-driven scheduler](0021-orchestrator-scheduler-as-single-threaded-event-loop.md) | Accepted                       |
| 0022 | [Persist state transitions before sending messages](0022-persist-state-transitions-before-sending.md)                      | Accepted                       |
| 0023 | [GPU algorithms in a standalone core library](0023-gpu-algorithms-in-standalone-core-library.md)                           | Accepted; see 0036, 0037       |
| 0024 | [Worker: separate session and compute threads, short GPU steps](0024-worker-separates-session-and-compute-threads.md)      | Accepted; extended by 0032     |
| 0025 | [Route geometry: legs re-routed on the CPU in FINALIZE](0025-route-geometry-by-cpu-leg-routing.md)                         | Accepted                       |
| 0026 | [Orchestrator implementation language: Python (asyncio)](0026-orchestrator-implementation-language.md)                     | Accepted                       |
| 0027 | [Graph file format: custom little-endian binary with a JSON manifest](0027-graph-file-format.md)                           | Accepted                       |
| 0028 | [Result artifact format: JSON, polylines in degrees](0028-result-artifact-format.md)                                       | Accepted                       |
| 0029 | [Object store: SeaweedFS instead of MinIO](0029-object-store-seaweedfs.md)                                                 | Accepted; see 0031             |
| 0030 | [Live map: workers route each new best plan on the roads](0030-live-map-from-island-bests.md)                              | Accepted                       |
| 0031 | [The orchestrator and the client run in Docker Compose](0031-orchestrator-and-client-in-compose.md)                        | Accepted; see 0038             |
| 0032 | [Bounded store retries; a lost GPU restarts its worker](0032-bounded-store-retries-and-lost-gpu-restart.md)                | Accepted                       |
| 0033 | [Distance batches hold one warp of 32 sources](0033-distance-batches-of-one-warp.md)                                       | Accepted                       |
| 0034 | [Islands exchange migrants on a 2-second timer](0034-migrant-exchange-on-a-timer.md)                                       | Accepted                       |
| 0035 | [GPU build targets: compute capability 6.1 and 12.0](0035-gpu-build-targets.md)                                            | Accepted                       |
| 0036 | [Orchestrator tests use a Python fake worker](0036-orchestrator-tests-use-a-python-fake-worker.md)                         | Accepted                       |
| 0037 | [A worker built without CUDA runs on the CPU](0037-worker-without-cuda-runs-on-the-cpu.md)                                 | Accepted; extended by 0039     |
| 0038 | [Users sign in to the client with their own tokens](0038-client-sign-in-with-named-tokens.md)                              | Accepted                       |
| 0039 | [CPU and GPU workers share jobs; each phase prefers its kind](0039-each-phase-prefers-its-kind-of-worker.md)               | Accepted                       |
