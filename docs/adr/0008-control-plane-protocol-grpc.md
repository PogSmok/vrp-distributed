# 0008. Control-plane protocol: gRPC

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** 0002, 0009, 0012, 0019, 0022, 0026

## Context

The orchestrator (Python, 0026), the workers (C++) and the client (Python)
need long-lived worker sessions and request-response calls, with one typed
contract. Workers are built separately, so a worker can be older or newer
than the orchestrator.

## Decision

All control-plane traffic uses gRPC with one shared `.proto` contract. The
contract has a major and minor protocol version, and changes within a major
version only add fields and messages. Peers send their protocol and build
version when they connect, and the orchestrator refuses a different major
version.

## Alternatives considered

- **REST with SSE or WebSocket:** no generated contract, and streaming needs
  an extra mechanism.
- **gRPC for workers, REST for the client:** two protocols, two contracts and
  two authentication paths.

## Consequences

- Gain: streaming, deadlines and cancellation built in; generated code for
  C++ and Python.
- Cost: `protoc` tooling in the C++ worker build, which is awkward on
  Windows; harder ad-hoc debugging than with `curl`.
- Revisit: plain REST for the client, if the gRPC tooling costs too much
  (the client only makes request-response calls, 0022).

## Confirmation

Build on every platform (0019) and run an end-to-end stream. A worker with
a different major protocol version is refused with an explicit error.
