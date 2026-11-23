"""The Migrant Pool: migrants of each job's current islands (ADR 0004).

The Scheduler says which islands a job has, and which attempt runs each,
through `SetPool`; the Worker API pushes and pulls migrants here directly,
bypassing the Scheduler's queue (ADR 0021). Nothing is persisted: the pool
starts empty after a restart (ADR 0014).
"""

import random
from collections.abc import Mapping

from vrp_orchestrator import vrp_pb2

type Owner = tuple[str, int]
"""The task and attempt that run an island."""


class MigrantPool:
    """The latest migrant of each island, by job."""

    def __init__(self, rng: random.Random) -> None:
        """Starts empty.

        Args:
            rng: Chooses which migrant a pull gets.
        """
        self._rng = rng
        self._owners: dict[str, dict[int, Owner]] = {}
        self._migrants: dict[str, dict[int, vrp_pb2.Solution]] = {}

    def set(self, job_id: str, islands: Mapping[int, Owner]) -> None:
        """Replaces a job's islands; none closes its pool.

        Args:
            job_id: The job.
            islands: Each island, with the attempt that runs it.
        """
        if not islands:
            self._owners.pop(job_id, None)
            self._migrants.pop(job_id, None)
            return
        self._owners[job_id] = dict(islands)
        kept = self._migrants.get(job_id, {})
        self._migrants[job_id] = {
            island: solution
            for island, solution in kept.items()
            if island in islands
        }

    def push(self, migrant: vrp_pb2.Migrant) -> bool:
        """Keeps an island's migrant, if its attempt runs that island.

        Args:
            migrant: From a worker.

        Returns:
            Whether it was kept.
        """
        job_id = self._job(migrant.attempt, migrant.island)
        if job_id is None:
            return False
        self._migrants[job_id][migrant.island] = migrant.solution
        return True

    def pull(self, request: vrp_pb2.MigrantRequest) -> vrp_pb2.Migrant | None:
        """A random migrant from another island of the requester's job.

        Args:
            request: From the attempt that runs `request.island`.

        Returns:
            The migrant, addressed to the requesting island, or None if the
            requester does not run that island or no other island has one.
        """
        job_id = self._job(request.attempt, request.island)
        if job_id is None:
            return None
        others = sorted(
            island
            for island in self._migrants[job_id]
            if island != request.island
        )
        if not others:
            return None
        source = self._rng.choice(others)
        return vrp_pb2.Migrant(
            attempt=request.attempt,
            island=request.island,
            solution=self._migrants[job_id][source],
        )

    def _job(self, attempt: vrp_pb2.Attempt, island: int) -> str | None:
        """The job whose `island` this attempt runs, if any."""
        owner = (attempt.task_id, attempt.attempt)
        for job_id, owners in self._owners.items():
            if owners.get(island) == owner:
                return job_id
        return None
