# 0010. Monitoring with Prometheus and Grafana

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** 0002, 0003

## Context

Operators need to observe system health: GPU utilisation, queue length, live
workers, take-over of stalled tasks, database and storage status. This is a
different audience and lifecycle from the per-job progress users see in the
client application. Monitoring must keep running when no client is connected
and must retain history.

## Decision drivers

- Independent of the observed system and of connected clients.
- History and alerting with little custom work.
- Metrics usable as benchmark evidence.

## Decision

We use Prometheus (collection and storage) and Grafana (dashboards). Our
processes expose `/metrics` endpoints, or standard exporters are used (for
PostgreSQL, GPUs and the object store), and Prometheus scrapes them directly.
The PostgreSQL exporter uses a read-only monitoring role (0003).
Workers are on the same network as Prometheus. Per-job telemetry stays in the
client application.

Modelling note: Prometheus and Grafana are drawn as external systems in the
architecture model because we do not build them. We do deploy them ourselves;
the deployment view shows that.

## Alternatives considered

### Option A: Build monitoring into the client application
- Pros: no extra tools.
- Cons: only runs while a client is open; no history or alerting; exposes
  internal cluster data to every client.
- Why rejected: wrong audience and lifecycle.

### Option B: OpenTelemetry collectors with push-based export
- Pros: vendor-neutral, supports traces as well as metrics, works across
  NAT because workers push outward.
- Cons: one more component per worker host; more configuration.
- Why rejected: workers share a network with Prometheus, so scraping is
  simpler. Revisit if workers must run behind NAT.

### Option C: Build a custom monitoring service
- Pros: tailored to our metrics.
- Cons: reimplements storage, querying and dashboards.
- Why rejected: the effort does not demonstrate the project's goals.

## Consequences

**What we gain:**
- Independent observability, history and alerting with little work.

**What we pay for it:**
- Instrumentation in C++ and extra services to run.
- GPU exporters differ by operating system and must be chosen with care.
- Scraping requires workers to be directly reachable from Prometheus, so
  workers behind NAT are not supported for metrics.

**Risks / to revisit later:**
- Restrict dashboard access to administrators.

## Confirmation

Every scrape target (orchestrator, workers, PostgreSQL exporter, object
store) shows as up in Prometheus, and the key dashboards load.
