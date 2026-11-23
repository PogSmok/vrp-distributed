"""The Worker Registry: connected workers and their send queues.

Each session has a bounded send queue; a worker that does not read its
messages fills it, and the session is closed rather than stall the
orchestrator (ADR 0021). The registry also lists the workers' metrics
addresses, for Prometheus to discover them (ADR 0010).
"""

import asyncio
import contextlib
import itertools
from dataclasses import dataclass, field
from typing import Final

from vrp_orchestrator import vrp_pb2

QUEUE_SIZE: Final = 1000
"""Messages waiting to be sent to one worker."""


@dataclass
class Session:
    """One worker's stream. `None` in the queue ends it."""

    id: int
    metrics_address: str
    queue: asyncio.Queue[vrp_pb2.OrchestratorMessage | None] = field(
        default_factory=lambda: asyncio.Queue(QUEUE_SIZE)
    )
    closed: bool = False


class Registry:
    """The current session of each worker."""

    def __init__(self) -> None:
        """Starts with no workers."""
        self._sessions: dict[str, Session] = {}
        self._ids = itertools.count()
        self.targets: tuple[str, ...] = ()
        """The metrics addresses, replaced as a whole when they change, so
        that the metrics thread can read them at any time."""

    @property
    def count(self) -> int:
        """How many workers have a session."""
        return len(self._sessions)

    def open(self, worker: str, metrics_address: str) -> Session:
        """Starts a worker's session, ending the one it replaces (ADR 0014).

        Args:
            worker: The worker's token name.
            metrics_address: The `host:port` of its `/metrics`.

        Returns:
            The session.
        """
        old = self._sessions.get(worker)
        if old is not None:
            _close(old)
        session = Session(next(self._ids), metrics_address)
        self._sessions[worker] = session
        self._update_targets()
        return session

    def close(self, worker: str, session: Session) -> None:
        """Forgets a session that ended, unless it was already replaced.

        Args:
            worker: The worker's token name.
            session: The session that ended.
        """
        _close(session)
        if self._sessions.get(worker) is session:
            del self._sessions[worker]
            self._update_targets()

    def send(self, worker: str, message: vrp_pb2.OrchestratorMessage) -> None:
        """Queues a message for a worker's current session.

        A worker with no session misses it; the lease timeout covers that
        (ADR 0022).

        Args:
            worker: The worker's token name.
            message: The message.
        """
        session = self._sessions.get(worker)
        if session is None or session.closed:
            return
        try:
            session.queue.put_nowait(message)
        except asyncio.QueueFull:
            _close(session)

    def _update_targets(self) -> None:
        self.targets = tuple(
            sorted(
                s.metrics_address
                for s in self._sessions.values()
                if s.metrics_address
            )
        )


def _close(session: Session) -> None:
    session.closed = True
    # A full queue: the stream sees `closed` once it reads what is queued.
    with contextlib.suppress(asyncio.QueueFull):
        session.queue.put_nowait(None)
