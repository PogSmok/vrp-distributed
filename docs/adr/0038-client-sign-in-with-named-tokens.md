# 0038. Users sign in to the client with their own tokens

- **Status:** Accepted
- **Date:** 2026-11-24
- **Related:** 0009, 0012, 0031

## Context

The client is opened from other machines in the lab. Under 0031 it had no
login and shared one `client` token, so it listened on localhost only, and
every user would have owned every job.

## Decision

- **Tokens.** Each user gets a named `client` token from the admin tool, as
  each worker does (0009). The client asks for it on a sign-in page and checks
  it with one call; the orchestrator remains the only place tokens are
  checked, and a job belongs to the token's name. The client holds the token
  for that browser tab, and keeps job views per token.
- **Transport.** The client serves HTTPS with the orchestrator's certificate
  from the lab CA, so it listens on `VRP_BIND` like the orchestrator.

## Alternatives considered

- **A reverse proxy with a password, such as Caddy:** one more service and a
  second set of credentials, and every user would still share one token.
- **Each user runs their own client:** a Python install on every machine.
- **OpenID Connect (Streamlit's `st.login`):** needs an identity provider,
  rejected in 0009.

## Consequences

- Gain: the client is usable from the lab, jobs are per user, and a user is
  revoked by deleting one line.
- Cost: a new tab or a reload asks for the token again, since Streamlit keeps
  nothing across sessions. Browsers warn about the certificate until they
  trust the lab CA. Streamlit's own TLS is not meant for heavy traffic, which
  a lab does not have.
- Revisit: if signing in on every reload annoys users, a signed session
  cookie behind a proxy.

## Confirmation

A worker token and an unknown token are refused at sign-in, and a user who
opens another user's job link sees that the job is not available.
