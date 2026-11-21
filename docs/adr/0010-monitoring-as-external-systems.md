# 0010. Monitoring with Prometheus and Grafana; logs collected by the orchestrator

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** 0002, 0003, 0009, 0014, 0019, 0024

## Context

Operators need system health with history (GPU use, queue length, workers,
database and storage), whether or not a client is connected. They also need
to follow one job across the orchestrator and up to 8 workers.

## Decision

- **Metrics.** Prometheus scrapes `/metrics` from our processes and from
  standard exporters. The orchestrator lists the connected workers on an
  `http_sd_configs` endpoint, so a new worker is scraped without changing
  any configuration. Grafana shows the dashboards. The model draws
  Prometheus and Grafana as external systems because we do not build them.
- **Logs.** Every process writes JSON-line logs tagged with job, task,
  attempt and worker. Workers send their lines in batches to the
  orchestrator over the session stream they already have (0002, 0024). The
  orchestrator writes them into its own log, which it rotates and keeps for
  30 days. Each worker also keeps a local log file, so lines that were never
  sent (for example before a crash) are not lost. Following a job means
  filtering the orchestrator's log by job ID.

## Alternatives considered

- **Monitoring inside the client:** runs only while a client is open; the
  wrong audience.
- **OpenTelemetry push:** more configuration than scraping; revisit if
  workers ever run behind NAT.
- **Loki, with a log agent on every machine:** a log server and up to 9
  agents to install, plus another unauthenticated port, for a lab of 8
  workers; revisit if the orchestrator's log becomes hard to search.
- **Log files on each machine only:** following one job would mean logging
  in to every machine.

## Consequences

- Gain: history and alerting with little work; one file holds everything a
  job did. Logs travel over the authenticated TLS stream, and no extra
  service is needed.
- Cost: instrumentation; log lines add traffic on the session stream;
  workers behind NAT cannot be scraped. Dashboards must be restricted to
  administrators.
- Cost: metric scrapes are plain, unauthenticated HTTP, so they are
  protected only by the firewall, which limits them to the lab network
  (0019). This is an accepted risk.

## Confirmation

Every scrape target is up, and a new worker is scraped without
configuration changes. Kill a worker during a job: filtering the
orchestrator's log by the job ID returns lines from the orchestrator and
from every worker that ran a task, and the killed worker's last lines are
in its local file.
