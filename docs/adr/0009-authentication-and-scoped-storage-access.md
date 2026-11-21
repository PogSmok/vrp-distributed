# 0009. Authentication of clients and workers; scoped storage access

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** 0002, 0007, 0008, 0012, 0016

## Context

The orchestrator is the system's entry point for both clients and workers.
Any host that can reach it could register as a worker and, for example,
poison the migrant pool (0004). Clients and workers also need access to
stored objects (graphs, distance matrices, results), but should not hold
object-store credentials.

## Decision drivers

- Encrypted transport on every control-plane connection.
- Identify and authorise both clients and workers.
- Workers and clients never hold long-lived storage credentials.
- No full user-account system.

## Decision

All control-plane connections use TLS. Every client, worker and administrator
presents a token with a role (`client`, `worker`, `admin`) that the
orchestrator validates on each call and when a stream is opened. Tokens are
issued by the administrator. Object-store access is granted only through
short-lived presigned URLs issued by the orchestrator for specific objects:
GET URLs for downloads (graphs, matrix rows, results) and PUT URLs for
uploads (matrix rows, result artifacts). Only the orchestrator and the
preprocessor hold object-store credentials.

A presigned URL's signature covers the host name. The orchestrator therefore
signs URLs for the object store's externally reachable address (configured
separately from its own internal address), and that address is served over
TLS.

## Alternatives considered

### Option A: Mutual TLS (client certificates)
- Pros: strong authentication, no tokens in requests.
- Cons: issuing and distributing certificates to every client and worker is
  laborious.
- Why rejected: operational overhead at this scale.

### Option B: OAuth 2.0 / OpenID Connect
- Pros: standard for user logins, supports external identity providers.
- Cons: requires an identity provider and substantial integration work.
- Why rejected: disproportionate for the project's scope.

### Option C: Shared object-store credentials on workers and clients
- Pros: simplest to implement.
- Cons: broad access from every machine, manual configuration, hard to rotate.
- Why rejected: contradicts self-service worker setup and least privilege.

## Consequences

**What we gain:**
- Encrypted transport, role-based access, and no storage credentials outside
  the orchestrator and preprocessor.

**What we pay for it:**
- Manual token management; no per-user identity.
- Presigned URL lifetime must cover the longest transfer.
- The object store must be reachable from client and worker machines, not
  only from the orchestrator.

**Risks / to revisit later:**
- A development setup may run without authentication; state this in the
  README.

## Confirmation

Try to register a worker with no token and with a `client` token; both must
fail. Verify that a presigned URL stops working after it expires and that it
cannot be used for another object. Download a presigned URL from a client
machine (not the server) to confirm that the signed host name is reachable.
