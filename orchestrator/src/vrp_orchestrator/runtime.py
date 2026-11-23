"""Runs the Scheduler on the event loop (ADR 0021).

One task takes events from the queue one at a time; each commit runs in a
task of its own and comes back as a `Committed` event. While the database is
unreachable, a commit is retried and the Scheduler is degraded: it refuses
new jobs, heartbeats go on in memory, and transitions wait (ADR 0014). Any
other error is a bug, and ends `run`, and with it the process.
"""

import asyncio
from collections.abc import Callable, Sequence
from typing import NoReturn, Protocol

import psycopg

from vrp_orchestrator.model import Row
from vrp_orchestrator.scheduler import (
    Action,
    CheckUpload,
    Commit,
    Committed,
    DeleteObjects,
    Event,
    Reply,
    Scheduler,
    Send,
    SetPool,
    Tick,
)


class Store(Protocol):
    """Where transitions are committed: the State Repository."""

    async def commit(self, rows: Sequence[Row]) -> None:
        """Writes one transition's rows in one transaction."""
        ...


class SchedulerLoop:
    """The Scheduler's queue, commits and timer."""

    def __init__(
        self,
        scheduler: Scheduler,
        store: Store,
        perform: Callable[[Send | CheckUpload | DeleteObjects | SetPool], None],
        *,
        tick_seconds: float = 1.0,
        retry_seconds: float = 1.0,
    ) -> None:
        """Runs `scheduler`, committing to `store`.

        Args:
            scheduler: The Scheduler.
            store: Where transitions are committed.
            perform: Runs a send, upload check, deletion or pool change;
                it must not block. Replies are delivered here.
            tick_seconds: How often time is reported to the Scheduler.
            retry_seconds: How long to wait before retrying a commit.
        """
        self._scheduler = scheduler
        self._store = store
        self._perform = perform
        self._tick_seconds = tick_seconds
        self._retry_seconds = retry_seconds
        self._queue: asyncio.Queue[Event] = asyncio.Queue()

    def post(self, event: Event) -> None:
        """Queues an event; the handlers of calls post theirs here.

        Args:
            event: What happened.
        """
        self._queue.put_nowait(event)

    async def run(self) -> NoReturn:
        """Handles events until a commit fails.

        An unreachable database is retried; any other error is a bug.

        Raises:
            ExceptionGroup: With the error of the failed commit.
        """
        async with asyncio.TaskGroup() as tasks:
            tasks.create_task(self._tick())
            self._run(tasks, self._scheduler.start())
            while True:
                event = await self._queue.get()
                self._run(tasks, self._scheduler.handle(event))

    def _run(self, tasks: asyncio.TaskGroup, actions: list[Action]) -> None:
        for action in actions:
            if isinstance(action, Commit):
                tasks.create_task(self._commit(action.rows))
            elif isinstance(action, Reply):
                action.respond(action.response)
            else:
                self._perform(action)

    async def _commit(self, rows: Sequence[Row]) -> None:
        while True:
            try:
                await self._store.commit(rows)
            except psycopg.OperationalError:
                self._scheduler.degraded = True
                await asyncio.sleep(self._retry_seconds)
                continue
            self._scheduler.degraded = False
            self.post(Committed())
            return

    async def _tick(self) -> NoReturn:
        while True:
            await asyncio.sleep(self._tick_seconds)
            self.post(Tick())
