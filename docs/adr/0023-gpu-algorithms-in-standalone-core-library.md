# 0023. GPU algorithms in a standalone core library

- **Status:** Accepted; test approach refined by 0036; the CPU worker uses
  the reference (0037)
- **Date:** 2026-11-21
- **Related:** 0006, 0011, 0017, 0018, 0024, 0025

## Context

The GPU algorithms are the riskiest work, and must be built and benchmarked
before the distributed system exists. CVRPLIB validation and the 1-GPU
baseline also need them on their own.

## Decision

The algorithms live in `vrp-core`, a library with no networking, no storage
and no threads of its own; callers supply callbacks and thread pools. The
Worker and a benchmark CLI use it. `vrp-core` also contains a simple CPU
reference for snapping, shortest paths and route evaluation. The reference
serves as a correctness oracle, for tests on machines without a GPU, and
behind a fake worker in orchestrator tests, but never in production.

## Alternatives considered

- **Algorithms inside the worker:** every experiment would need the whole
  system running.
- **A separate executable per algorithm:** reloads the graph for every task.

## Consequences

- Gain: kernel work can start at once; benchmarks use production code; GPU
  results are checked against an independent implementation.
- Cost: a library interface to keep stable, and a second, simple
  implementation to maintain.

## Confirmation

`vrp-core` builds without gRPC or libcurl, and its CPU tests pass without a
GPU. GPU distances match CPU Dijkstra. The CLI solves a CVRPLIB instance
and a road problem from files.
