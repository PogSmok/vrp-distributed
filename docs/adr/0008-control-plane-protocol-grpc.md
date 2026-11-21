# 0008. Control-plane protocol: gRPC

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** 0002, 0003, 0009, 0012

## Context

The orchestrator talks to the client application and to workers.
Interactions include long-lived streams (progress updates, heartbeats,
assignments) and request-response calls (submit job, query status). The
orchestrator is written in C++, the client in Python. The client is a local
application, not a browser, so browser limitations on gRPC do not apply.

## Decision drivers

- Bidirectional streaming for assignments, heartbeats and progress.
- One typed contract shared by C++ and Python, with explicit versioning.
- Deadlines and cancellation for long operations.

## Decision

We use gRPC with Protobuf for all control-plane communication, with a single
`.proto` contract shared by the orchestrator, workers and client.

## Alternatives considered

### Option A: REST over HTTP/JSON (plus SSE or WebSocket for streaming)
- Pros: simple, easy to debug with `curl`, fewer build tools in C++.
- Cons: streaming needs extra mechanisms; no generated, typed contract
  between C++ and Python.
- Why rejected: streaming and a shared typed contract are central needs.

### Option B: gRPC for workers, REST for the client
- Pros: friendlier client interface and tooling.
- Cons: two protocols, two contracts and two authentication paths in the
  orchestrator.
- Why rejected: one protocol is simpler to build and to secure.

## Consequences

**What we gain:**
- Built-in streaming, deadlines and cancellation; generated code from one
  contract; explicit compatibility rules.

**What we pay for it:**
- More build tooling (`protoc`, plugins), which can be awkward in C++ builds
  on Windows. Harder ad-hoc debugging than with `curl`.

**Risks / to revisit later:**
- If the gRPC build setup costs too much time, REST with SSE is an acceptable
  fallback for the client-facing interface.

## Confirmation

Build the orchestrator and a worker with gRPC on all target platforms (see
the planned deployment ADR) and run an end-to-end stream before depending on
this decision further.
