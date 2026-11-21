# 0026. Orchestrator implementation language: Python (asyncio)

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** 0008, 0019, 0021, 0023

## Context

The orchestrator is an I/O-bound control plane: gRPC, PostgreSQL, URL
signing and the Scheduler, under a small load. It shares only the `.proto`
contract with the C++ worker, and the project already uses Python.

## Decision

The orchestrator is written in Python. It uses the asyncio API of `grpcio`,
`psycopg` 3, and an S3 library for presigned URLs, with mypy as part of the
build. The asyncio event loop runs the Scheduler (0021).

## Alternatives considered

- **C++:** a hard asynchronous gRPC API, and extra libraries for SQL and URL
  signing; slower to write and test, and its speed is not needed.
- **Go:** a good fit, but a third language.

## Consequences

- Gain: much less code, plain `pytest`, and no C++ toolchain on the server.
- Cost: per-message overhead, to be measured with migrant traffic; type
  errors appear at run time unless mypy is enforced.
- Revisit: move the Migrant Pool into its own process if the loop is too
  slow.

## Confirmation

The load test from 0021, with 8 simulated workers exchanging migrants,
keeps heartbeat handling under 100 ms. The orchestrator runs on Windows and
Linux.
