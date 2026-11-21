# Architecture Decision Records

This directory contains the Architecture Decision Records (ADRs).
An ADR is a short document that captures **one** architectural decision.
Diagrams (see `docs/architecture/`) show *what* the system looks like.
ADRs explain *why* it looks the way it does.

Quantitative targets that ADRs refer to (number of workers, problem size,
graph size, acceptable downtime) live in `docs/quality-attributes.md`.

## When to write an ADR

Write one when a decision:
- is hard or expensive to reverse (protocols, data stores, data distribution)
- has realistic alternatives that someone could reasonably ask about
- affects more than one component

Do not write ADRs for trivial choices (naming, formatting, small refactors)
or for details of a single, easily replaced component. Record those in the
README of that component instead.

## How to add a new ADR

1. Copy `0000-template.md` to `NNNN-short-title.md`, where `NNNN` is the next
   free number (zero-padded) and the title is lowercase with hyphens, e.g.
   `0123-example-decision.md`.
2. Fill in every section. All sections are mandatory, except those the
   template explicitly marks as optional.
3. Write the real alternatives (usually at least two; do not add filler to
   reach a count) and a *Confirmation* section that says how you will check
   that the decision works.
4. Set `Status: Proposed` while the decision is still open, and change it to
   `Accepted` once the decision is made. Accepted means decided, not proven:
   the *Confirmation* section says how it will be checked, and if that check
   fails, a new ADR supersedes this one.
5. Add a row to the index table below.
6. Commit the ADR together with the change it relates to (code or diagrams),
   so the decision and its implementation share history.
7. If the decision affects the architecture diagrams, update
   `docs/architecture/workspace.dsl` in the same commit.

## How to change or reverse a decision

Do not rewrite an accepted ADR. Instead:
1. Write a new ADR describing the new decision and referencing the old one
   in `Related`.
2. Change the old ADR's status to `Superseded by NNNN`.
3. Update the index table.

Small corrections (typos, broken links) can be made in place.

## Conventions

- One file per decision.
- Titles state the decision, not the question ("Control-plane protocol:
  gRPC", not "gRPC vs REST?").
- Numbers are never reused or renumbered once merged.
- Keep ADRs short.
- Write in the present tense ("We use X because ...").

## Index

| #    | Title                                                                                                            | Status   |
|------|------------------------------------------------------------------------------------------------------------------|----------|
| 0001 | [Job and state store: PostgreSQL](0001-job-and-state-store-postgresql.md)                                        | Accepted |
| 0002 | [Job assignment over worker-initiated gRPC streams](0002-job-assignment-over-worker-initiated-streams.md)        | Accepted |
| 0003 | [Orchestrator as the single owner of job state](0003-orchestrator-owns-job-state.md)                             | Accepted |
| 0004 | [Island migration via orchestrator-held migrant pool](0004-island-migration-via-migrant-pool.md)                 | Accepted |
| 0005 | [Offline import of regional OSM extracts](0005-offline-import-of-osm-extracts.md)                                | Accepted |
| 0006 | [Problem input format: custom JSON](0006-problem-input-format.md)                                                | Accepted |
| 0007 | [Graph distribution: object store with local cache](0007-graph-distribution-object-store-with-local-cache.md)    | Accepted |
| 0008 | [Control-plane protocol: gRPC](0008-control-plane-protocol-grpc.md)                                              | Accepted |
| 0009 | [Authentication of clients and workers; scoped storage access](0009-authentication-and-scoped-storage-access.md) | Accepted |
| 0010 | [Monitoring with Prometheus and Grafana](0010-monitoring-as-external-systems.md)                                 | Accepted |
| 0011 | [Work split across GPUs: full graph per worker, two-phase pipeline](0011-work-split-across-gpus.md)              | Accepted |
| 0012 | [No separate API layer; orchestrator exposes the client interface](0012-no-separate-api-layer.md)                | Accepted |
| 0013 | [Job and task model with group scheduling](0013-job-and-task-model-with-group-scheduling.md)                     | Accepted |
| 0014 | [Failure and recovery model](0014-failure-and-recovery-model.md)                                                 | Accepted |
| 0015 | [Graph versioning and catalogue](0015-graph-versioning-and-catalogue.md)                                         | Accepted |
| 0016 | [Result storage and retrieval](0016-result-storage-and-retrieval.md)                                             | Accepted |

### Planned (need experiments first)

| #    | Title                                                       |
|------|-------------------------------------------------------------|
| 0017 | GPU distance algorithm (shortest paths from many sources)   |
| 0018 | Solver algorithm (local search and genetic operators)       |
| 0019 | Deployment topology and build platforms (Windows and Linux) |
| 0020 | Edge cost: road length or travel time                       |
