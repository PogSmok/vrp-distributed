# 0012. No separate API layer; orchestrator exposes the client interface

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** 0003, 0008, 0009, 0016

## Context

An early design had a web frontend, a Python HTTPS API and the orchestrator.
The client is now a local application (Python / Streamlit) that connects
directly to the orchestrator. A separate API layer would add a hop, an
additional service and another contract to maintain.

## Decision drivers

- Fewer moving parts for a single type of client.
- One contract and one authentication path (0008, 0009).

## Decision

There is no separate API service. The orchestrator exposes the client-facing
gRPC interface itself, including input validation and authentication.

## Alternatives considered

### Option A: Separate stateless API service (for example Python / FastAPI)
- Pros: independent scaling; validation and authentication in one place;
  stable HTTP contract; replaceable frontend.
- Cons: extra service, extra hop, a second contract to version.
- Why rejected: with a single local client, the benefits do not justify the
  added complexity.

### Option B: A gateway or proxy in front of the orchestrator (for example Envoy)
- Pros: TLS termination and token validation outside the C++ code.
- Cons: one more component to configure and monitor.
- Why rejected: authentication is simple enough to implement in the
  orchestrator for now.

## Consequences

**What we gain:**
- Fewer moving parts and one contract.

**What we pay for it:**
- The C++ orchestrator validates and authenticates client input and is
  directly reachable by clients.
- Clients also reach the object store directly, but only to download results
  through presigned URLs issued by the orchestrator (0009, 0016).
- A different type of client (for example a web UI) would need a gateway.

**Risks / to revisit later:**
- Revisit if a browser-based client is added.

## Confirmation

Review check: the architecture model has no API container, the orchestrator
is the only container the client connects to for control traffic, and the
client's only other connection is the presigned download from the object
store.
