# Documentation

Start with the [root README](../README.md), then [setup](setup.md) to run it.

## Guides

| Document                                           | What it answers                                                      |
|----------------------------------------------------|----------------------------------------------------------------------|
| [setup.md](setup.md)                               | How to go from a clean machine to a solved job                       |
| [how-the-solver-works.md](how-the-solver-works.md) | How the search finds routes, and what more GPUs change               |
| [failure-modes.md](failure-modes.md)               | What happens when a worker, the GPU, the database or the store fails |
| [quality-attributes.md](quality-attributes.md)     | The targets the design is held to, and what has been measured        |

## Decisions

[adr/README.md](adr/README.md) indexes the architecture decision records,
and explains how to add or supersede one. The architecture model is
[architecture/workspace.dsl](architecture/workspace.dsl), a Structurizr
workspace with context, container, component and deployment views, which
[architecture/README.md](architecture/README.md) shows as Mermaid diagrams.
`python docs/architecture/export.py` exports them again after a change to the
model, and CI fails if they no longer match it.

## Formats and contracts

| Document                                   | Contract between                                      |
|--------------------------------------------|-------------------------------------------------------|
| [formats/problem.md](formats/problem.md)   | Whoever writes problems, and the client               |
| [formats/graph.md](formats/graph.md)       | The preprocessor, which writes graphs, and vrp-core   |
| [formats/protocol.md](formats/protocol.md) | The orchestrator, the workers and the client (gRPC)   |
| [formats/result.md](formats/result.md)     | FINALIZE, which writes results, and the client        |
| [formats/database.md](formats/database.md) | The orchestrator and its PostgreSQL schema, generated |

`proto/vrp/v1/vrp.proto` defines every message. The golden files beside the
format specs are what both sides are tested against.

## Components

| README                                              | Component                                                       |
|-----------------------------------------------------|-----------------------------------------------------------------|
| [core/README.md](../core/README.md)                 | vrp-core: graph reader, snapping, GPU distances, the solver     |
| [worker/README.md](../worker/README.md)             | vrp-worker: session and compute threads, transfers, graph cache |
| [orchestrator/README.md](../orchestrator/README.md) | vrp-orchestrator: scheduler, services, storage, telemetry       |
| [client/README.md](../client/README.md)             | vrp-client: the Streamlit app and its live map                  |
| [preprocessor/README.md](../preprocessor/README.md) | vrp-preprocessor: OSM import, synthetic graphs, problems        |
| [bench/README.md](../bench/README.md)               | vrp-bench: runs vrp-core on files and times each phase          |
| [deploy/README.md](../deploy/README.md)             | The Compose stack, workers as services, backups, the demo       |

## Images

[images/](images/) holds the diagrams, as SVG for GitHub's light and dark
themes, and screenshots of the client.
