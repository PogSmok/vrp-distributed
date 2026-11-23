"""Logs, metrics and worker discovery (ADR 0010).

The orchestrator writes JSON-line logs, rotated daily and kept 30 days,
and workers' log lines go into the same file, tagged with the worker, so
that one file holds everything a job did. An HTTP server on the metrics
port serves `/metrics` to Prometheus, and `/workers`, the workers' metrics
addresses in the format of Prometheus's `http_sd_configs`.
"""

import datetime as dt
import json
import logging
import threading
from collections.abc import Callable, Iterable
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from logging.handlers import TimedRotatingFileHandler
from pathlib import Path
from typing import Final, override

from prometheus_client import (
    CONTENT_TYPE_LATEST,
    Counter,
    Gauge,
    Histogram,
    generate_latest,
)

LOG_DAYS: Final = 30
"""How long log files are kept (ADR 0010)."""

WORKERS: Final = Gauge("vrp_workers_connected", "Workers with a session.")
JOBS: Final = Gauge("vrp_jobs", "Jobs, by phase.", ["phase"])
WAITING: Final = Gauge(
    "vrp_scheduler_waiting_events", "Events waiting for a commit."
)
COMMITS: Final = Histogram(
    "vrp_commit_seconds", "How long a transition takes to commit."
)
EVENTS: Final = Histogram(
    "vrp_event_seconds", "How long the Scheduler takes for one event."
)
MIGRANTS: Final = Counter(
    "vrp_migrants_total",
    "Migrants pushed, by whether they were kept.",
    ["kept"],
)

log: Final = logging.getLogger("vrp")


class _JsonFormatter(logging.Formatter):
    @override
    def format(self, record: logging.LogRecord) -> str:
        if record.name == "vrp.workers":
            return record.getMessage()  # a worker's line, already JSON
        entry: dict[str, object] = {
            # UTC, so that lines from every machine sort together.
            "time": dt.datetime.fromtimestamp(record.created, dt.UTC).strftime(
                "%Y-%m-%dT%H:%M:%SZ"
            ),
            "level": record.levelname,
            "message": record.getMessage(),
        }
        entry.update(getattr(record, "fields", {}))
        if record.exc_info:
            entry["error"] = self.formatException(record.exc_info)
        return json.dumps(entry)


def setup_logging(directory: Path) -> None:
    """Sends every log line to a daily file in `directory`, and to stderr.

    Args:
        directory: Where the log files go; created if missing.
    """
    directory.mkdir(parents=True, exist_ok=True)
    formatter = _JsonFormatter()
    handlers: list[logging.Handler] = [
        TimedRotatingFileHandler(
            directory / "orchestrator.log",
            when="midnight",
            backupCount=LOG_DAYS,
            encoding="utf-8",
            utc=True,
        ),
        logging.StreamHandler(),
    ]
    for handler in handlers:
        handler.setFormatter(formatter)
    logging.basicConfig(level=logging.INFO, handlers=handlers, force=True)


def worker_lines(worker: str, lines: Iterable[str]) -> None:
    """Writes a worker's log lines into the orchestrator's log.

    Each line is tagged with the worker, so that filtering by a job ID finds
    the lines of every worker that ran its tasks.

    Args:
        worker: The worker's token name.
        lines: JSON-line records; others are kept as messages.
    """
    logger = logging.getLogger("vrp.workers")
    for line in lines:
        try:
            entry = json.loads(line)
        except ValueError:
            entry = None
        if not isinstance(entry, dict):
            entry = {"message": line}
        entry["worker"] = worker
        logger.info(json.dumps(entry))


def serve_metrics(
    address: str, targets: Callable[[], tuple[str, ...]]
) -> ThreadingHTTPServer:
    """Serves `/metrics` and `/workers` from a thread of its own.

    Args:
        address: The `host:port` to listen on.
        targets: The workers' metrics addresses; called from that thread,
            so it must only read a value replaced as a whole.

    Returns:
        The running server; `shutdown()` and then `server_close()` stop it.
    """

    class Handler(BaseHTTPRequestHandler):
        @override
        def log_message(self, format: str, *args: object) -> None:
            pass  # scrapes are too frequent to log

        def do_GET(self) -> None:
            if self.path == "/metrics":
                self._reply(CONTENT_TYPE_LATEST, generate_latest())
            elif self.path == "/workers":
                body = json.dumps([{"targets": list(targets()), "labels": {}}])
                self._reply("application/json", body.encode())
            else:
                self.send_error(404)

        def _reply(self, content_type: str, body: bytes) -> None:
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    host, _, port = address.rpartition(":")
    server = ThreadingHTTPServer((host, int(port)), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server
