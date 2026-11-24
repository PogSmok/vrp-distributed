# 0031. The orchestrator and the client run in Docker Compose

- **Status:** Accepted
- **Date:** 2026-11-23
- **Related:** 0009, 0010, 0012, 0019, 0029

## Context

ADR 0019 ran the orchestrator as a native service and the client on a
dispatcher's machine. Starting the control plane then took several manual
installs, on a project whose first impression is its client.

## Decision

- **One Compose file** runs PostgreSQL, SeaweedFS, the orchestrator,
  Prometheus, Grafana, the client, the metrics exporter and the nightly
  backup, from one image build and pinned third-party images.
- **Workers stay native** for fair GPU benchmarks (0019), as a systemd or
  WinSW service from the files in `deploy/worker/`.
- **Secrets.** A one-shot service creates the lab CA, certificates, tokens and
  passwords in a Docker volume on first start; nothing secret is in the
  repository. An `admin` tool hands out worker configurations and registers
  graphs.
- **Network.** Every port listens on localhost unless `VRP_BIND` opens the
  orchestrator, the store's TLS port and Grafana to the lab, behind the
  firewall rules of 0019. The client and Prometheus have no login, so they
  always stay on localhost.

## Alternatives considered

- **Native services (0019):** a different manual setup on each operating
  system.
- **Kubernetes:** disproportionate for one server.

## Consequences

- Gain: the whole control plane starts with one command.
- Cost: an image to build when our code changes.
- Revisit: a login on the client, if it must be reachable from the lab.

## Confirmation

CI starts the stack from scratch and waits for every service to be healthy.
A native worker registers with it over TLS and runs a job end to end.
