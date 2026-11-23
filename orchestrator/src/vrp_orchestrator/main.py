"""Runs the orchestrator: `vrp-orchestrator --config orchestrator.toml`."""

import argparse
import asyncio
import contextlib
import datetime as dt
import random
import signal
import sys
import uuid
from collections.abc import Coroutine, Sequence
from pathlib import Path
from typing import Final

import grpc

from vrp_orchestrator.artifacts import ArtifactManager
from vrp_orchestrator.auth import AuthInterceptor, TokenFile, server_credentials
from vrp_orchestrator.config import Config, load_config
from vrp_orchestrator.model import Phase
from vrp_orchestrator.pool import MigrantPool
from vrp_orchestrator.registry import Registry
from vrp_orchestrator.repository import Repository
from vrp_orchestrator.runtime import SchedulerLoop
from vrp_orchestrator.scheduler import (
    CheckUpload,
    DeleteObjects,
    Scheduler,
    Send,
    SetPool,
    Settings,
    UploadChecked,
)
from vrp_orchestrator.services import ROLES, Services
from vrp_orchestrator.telemetry import (
    JOBS,
    WORKERS,
    log,
    serve_metrics,
    setup_logging,
)

_EXPIRY_SECONDS: Final = 24 * 3600
_GAUGE_SECONDS: Final = 5.0
_STOP_SECONDS: Final = 5.0
_MIN_PING_MS: Final = 5000


def main(argv: Sequence[str] | None = None) -> int:
    """Runs the orchestrator until it is stopped.

    Args:
        argv: The arguments, without the program name.

    Returns:
        The process exit code: 1 if the configuration is invalid, 0 once
        stopped by SIGTERM or Ctrl+C.
    """
    parser = argparse.ArgumentParser(
        prog="vrp-orchestrator", allow_abbrev=False
    )
    parser.add_argument("--config", type=Path, required=True)
    arguments = parser.parse_args(argv)
    try:
        config = load_config(arguments.config)
    except (OSError, ValueError) as error:
        sys.stderr.write(f"error: {error}\n")
        return 1
    setup_logging(config.log_dir)
    # psycopg's asynchronous connections need a selector loop on Windows;
    # grpc.aio runs on it too.
    asyncio.run(serve(config), loop_factory=asyncio.SelectorEventLoop)
    return 0


async def serve(config: Config) -> None:
    """Starts every part of the orchestrator, and runs the Scheduler.

    It stops, closing what it opened, when cancelled or sent SIGTERM.
    Nothing more is needed: a restart recovers from the database (ADR 0014).

    Args:
        config: The configuration.
    """
    repository = Repository(config.database)
    await repository.migrate()
    rows = await repository.load()
    artifacts = ArtifactManager(config.storage)
    await artifacts.ensure_bucket()
    settings = Settings()
    scheduler = Scheduler(
        rows,
        settings=settings,
        clock=lambda: dt.datetime.now(dt.UTC),
        new_id=lambda: uuid.uuid4().hex,
        store=artifacts,
    )
    registry = Registry()
    pool = MigrantPool(random.Random())  # noqa: S311  # not for secrets
    background: set[asyncio.Task[None]] = set()

    def spawn(work: Coroutine[object, object, None]) -> None:
        task = asyncio.create_task(_logged(work))
        background.add(task)
        task.add_done_callback(background.discard)

    async def check(effect: CheckUpload) -> None:
        # The Scheduler waits as long for the result (a firm deadline).
        try:
            async with asyncio.timeout(settings.lease_timeout.total_seconds()):
                error = await artifacts.check_upload(effect.key, effect.upload)
        except TimeoutError:
            error = "the object store is unreachable"
        loop.post(UploadChecked(effect.task_id, effect.attempt, error))

    def perform(effect: Send | CheckUpload | DeleteObjects | SetPool) -> None:
        if isinstance(effect, Send):
            registry.send(effect.worker, effect.message)
        elif isinstance(effect, CheckUpload):
            spawn(check(effect))
        elif isinstance(effect, DeleteObjects):
            spawn(artifacts.delete(effect.keys))
        else:
            pool.set(effect.job_id, effect.islands)

    loop = SchedulerLoop(scheduler, repository, perform)
    server = _grpc_server(
        config, Services(loop.post, registry, pool, artifacts)
    )
    metrics = serve_metrics(config.metrics, lambda: registry.targets)
    spawn(_expire(artifacts))
    spawn(_gauges(scheduler, registry))
    await server.start()
    log.info("listening on %s, metrics on %s", config.listen, config.metrics)
    try:
        await _run_until_stopped(loop)
    finally:
        await server.stop(_STOP_SECONDS)
        metrics.shutdown()
        metrics.server_close()
        await repository.close()


def _grpc_server(config: Config, services: Services) -> grpc.aio.Server:
    server = grpc.aio.server(
        interceptors=[AuthInterceptor(TokenFile(config.tokens), ROLES)],
        # Workers ping an otherwise quiet stream to notice a lost connection
        # within a lease; by default, gRPC would close it for doing so.
        options=[
            ("grpc.http2.min_ping_interval_without_data_ms", _MIN_PING_MS)
        ],
    )
    server.add_generic_rpc_handlers(services.handlers())
    if config.certificate is not None and config.key is not None:
        credentials = server_credentials(config.certificate, config.key)
        server.add_secure_port(config.listen, credentials)
    else:
        log.warning("serving without TLS: for local development only")
        server.add_insecure_port(config.listen)
    return server


async def _run_until_stopped(loop: SchedulerLoop) -> None:
    """Runs the Scheduler until cancelled, as by Ctrl+C, or sent SIGTERM."""
    run = asyncio.create_task(loop.run())
    # Windows, where the orchestrator runs only for development, has none.
    with contextlib.suppress(NotImplementedError):
        asyncio.get_running_loop().add_signal_handler(
            signal.SIGTERM, run.cancel
        )
    try:
        await run
    except asyncio.CancelledError:
        log.info("stopping")


async def _logged(work: Coroutine[object, object, None]) -> None:
    try:
        await work
    except Exception:  # noqa: BLE001  # logged; the orchestrator goes on
        log.exception("a background task failed")


async def _expire(artifacts: ArtifactManager) -> None:
    """Deletes expired results and matrices once a day (ADR 0016)."""
    while True:
        try:
            deleted = await artifacts.expire(dt.datetime.now(dt.UTC))
        except Exception:  # noqa: BLE001  # logged; the next run catches up
            log.exception("deleting expired objects failed")
        else:
            log.info("deleted %d expired objects", deleted)
        await asyncio.sleep(_EXPIRY_SECONDS)


async def _gauges(scheduler: Scheduler, registry: Registry) -> None:
    """Updates the gauges from the event loop, which owns the state."""
    while True:
        WORKERS.set(registry.count)
        for phase in Phase:
            count = sum(job.phase is phase for job in scheduler.jobs.values())
            JOBS.labels(phase=phase.value).set(count)
        await asyncio.sleep(_GAUGE_SECONDS)
