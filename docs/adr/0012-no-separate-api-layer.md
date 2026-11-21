# 0012. No separate API layer; orchestrator exposes the client interface

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** 0003, 0008, 0009, 0016

## Context

An early design had a web frontend, a Python HTTPS API and the orchestrator.
The client is now a local Streamlit application, and an API layer would add
a hop, a service and a second contract.

## Decision

There is no separate API service. The orchestrator exposes the client gRPC
interface itself, including validation and authentication. Clients reach
the object store only to download results through presigned URLs.

## Alternatives considered

- **A stateless API service (FastAPI):** independent scaling is not worth a
  second service for one client.
- **A gateway (Envoy):** another component, and authentication is simple
  enough to do in the orchestrator.

## Consequences

- Gain: fewer moving parts and one contract.
- Cost: clients reach the orchestrator directly.
- Revisit: a browser-based client would need a gateway.

## Confirmation

Review check: the model has no API container, and the client connects only
to the orchestrator and, for presigned downloads, to the object store.
