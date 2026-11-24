# 0036. Orchestrator tests use a Python fake worker

- **Status:** Accepted
- **Date:** 2026-11-24
- **Related:** 0021, 0023

## Context

ADR 0023 expected vrp-core's CPU reference to sit behind a fake worker in
the orchestrator's tests. The orchestrator is Python and its tests run in
every CI job; a fake worker in C++ would need the worker's build there.

## Decision

The orchestrator's tests drive it with workers written in Python, which
follow the protocol but compute nothing: the Scheduler's tests through its
events, and `test_a_job_runs_end_to_end_over_grpc` over real gRPC. vrp-core's
reference stays the oracle of the C++ tests.

## Alternatives considered

- **The C++ worker on the CPU reference (0023):** a C++ toolchain and vcpkg
  in every Python job, to test scheduling that does not depend on results.

## Consequences

- Gain: fast orchestrator tests with no C++ build.
- Cost: no test runs the real worker against the real orchestrator; that is
  what the integration tests of the next PR add.

## Confirmation

The Python CI jobs run the Scheduler, services and runtime tests on Windows
and Linux without a C++ toolchain.
