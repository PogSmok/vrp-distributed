# 0009. Authentication of clients and workers; scoped storage access

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** 0004, 0007, 0013, 0014, 0016, 0019

## Context

Any host that can reach the orchestrator could pose as a worker (and, for
example, poison the migrant pool) or as a client. Workers and clients need
stored objects, but should not hold storage credentials.

## Decision

- **Transport and tokens.** All control-plane connections use TLS. Every
  client, worker and administrator has its own named token with a role
  (`client`, `worker`, `admin`). The orchestrator keeps the hash, name and
  role of each token in a file, and re-reads it when the file's
  modification time changes. Deleting a line revokes that token without a
  restart.
- **Workers.** A worker's identity is its token name. Leases belong to that
  name, a new session replaces any old one, and reports are accepted only
  from the worker an attempt is assigned to (0013, 0014).
- **Clients.** Only the client that submitted a job, or an administrator,
  can poll, cancel or fetch it.
- **Storage.** Objects are reachable only through short-lived presigned URLs
  for specific objects, signed for the object store's external TLS address.
  A worker can ask for a fresh URL for its current attempt. Only the
  orchestrator and the preprocessor hold storage credentials.

## Alternatives considered

- **Mutual TLS:** distributing certificates to every machine is laborious.
- **OAuth or OpenID Connect:** needs an identity provider; disproportionate.
- **Shared storage credentials:** broad access, and hard to rotate.

## Consequences

- Gain: encrypted traffic, least privilege, and revocation per identity.
- Cost: manual token management; the object store must be reachable from
  clients and workers. Running without TLS and tokens is allowed only for
  local development, must be switched on explicitly, and is logged at
  start-up.

## Confirmation

Negative tests, all rejected: no token, a wrong role, a revoked token, a
report for another worker's attempt, another client's job, an expired URL,
and a URL used for another object. A presigned URL downloads from a client
machine, which proves that it was signed for the external host name and not
the internal one.
