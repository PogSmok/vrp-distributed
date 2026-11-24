# Failure modes

What happens when part of the system fails during a job, what is lost, and
what backs the claim. The rules are in ADR 0013, 0014, 0022 and 0032, and the
timeouts in `docs/formats/protocol.md`.

**Evidence** names a committed test where there is one. **Verified** says how
far the behaviour has been checked: by those tests; by hand, with real
processes on the development machine and a harness that is not committed
(2026-11-23 and 24), before the live road plans and the shared GPU matrix of
PR 9 were added; or not at all. Runs on several machines, and committed
versions of the hand checks, come with the integration tests of the next PR.

## A worker dies

The process is killed, the machine turns off, or its network goes.

- **What happens.** Its session ends or falls silent. With no heartbeat for
  30 s, its leases expire. A PREPARE, DISTANCE or FINALIZE task is given to
  another connected worker as its next attempt; after 3 failed attempts the
  job fails with the last reason. A SOLVE task is not given out again: its
  islands are dropped, but their best plans, saved every 3 s, still count for
  FINALIZE. If every island is lost while time remains, SOLVE restarts once,
  seeded with the saved plans.
- **What is lost.** The running attempt's work, up to 3 s of each lost
  island's progress, and migrants in flight. A late upload from the lost
  attempt is ignored, as uploads are keyed by attempt.
- **Evidence.** ADR 0013, 0014. `test_leases_expire_and_old_attempts_are_ignored`,
  `test_a_retry_goes_to_another_idle_worker`,
  `test_lost_islands_restart_once_then_finalize_from_saved_solutions`, and
  `test_every_job_ends`, which generates random mixes of failures, lost
  connections and timeouts (all in `orchestrator/tests/test_scheduler.py`).
- **Verified.** By those tests, against the Scheduler alone. Killing a real
  worker was checked by hand. Not verified on several machines.

## A worker loses its GPU

A driver reset or a GPU fault makes CUDA fail every call until the process
restarts.

- **What happens.** The CUDA errors documented as needing a restart become
  `kDeviceLost`. The worker reports its task as failed and exits with an
  error, and its service manager starts it again (ADR 0032). The task is
  then retried as above. A worker that stops making progress for 60 s exits
  the same way.
- **What is lost.** As for a dead worker.
- **Evidence.** ADR 0024, 0032; `core/src/gpu/device_buffer.cu` lists the
  errors.
- **Verified.** By hand: with an injected launch failure, the worker exited
  and the job finished on the other worker. No committed test injects a CUDA
  fault, and the systemd and WinSW services that restart a worker have not
  been installed and tried.

## The orchestrator restarts

- **What happens.** It reloads every job, task and island from PostgreSQL
  and gives every lease one more 30 s. Workers reconnect within 5 s, list the
  attempts they still hold, and resend the reports that were not confirmed.
  Attempts nobody lists fail and are retried. A solve phase keeps its time
  limit, which counts from the start stored in the database.
- **What is lost.** Nothing committed. The migrant pool and the latest road
  plan for the live map are in memory only, so they start empty.
- **Evidence.** ADR 0014, 0021, 0022.
  `test_a_restart_gives_leases_a_grace_period_and_keeps_listed_ones`,
  `test_a_restart_past_the_time_limit_moves_on`
  (`test_scheduler.py`), and `test_committed_rows_are_read_back_as_they_were`
  (`test_repository.py`, against PostgreSQL).
- **Verified.** By those tests. A restart in the middle of a real job was
  checked by hand.

## The database is down

- **What happens.** The orchestrator retries its commit every second and is
  degraded meanwhile: new jobs are refused with `UNAVAILABLE`, heartbeats and
  polls are answered from memory, and every other change waits, so nothing is
  assigned or confirmed. Workers keep their leases and their unconfirmed
  reports. If more than 10,000 events wait, the orchestrator exits, and
  recovers from the database once it is back.
- **What is lost.** Nothing committed. Jobs submitted during the outage are
  refused, not queued.
- **Evidence.** ADR 0014, 0021.
  `test_a_database_outage_degrades_the_scheduler_until_it_is_back`
  (`test_runtime.py`), `test_a_full_waiting_queue_refuses_clients_then_stops`
  (`test_scheduler.py`).
- **Verified.** By those tests, with a simulated outage. Not with a real
  PostgreSQL stopped during a job.

## The object store is down

- **What happens.** Workers retry their downloads and uploads with backoff of
  up to 15 s, log the reason once, and fail the attempt after 10 minutes; the
  attempt limit then fails the job with that reason, after about 30 minutes.
  The orchestrator's check of an upload gives up after 30 s and fails that
  attempt. The client retries a result download at its next poll.
- **What is lost.** The attempts that ran out of time. Objects already stored
  stay in the store's volume.
- **Evidence.** ADR 0014, 0032. `test_uploads_are_checked_against_what_the_worker_reported`
  (`test_artifacts.py`, against SeaweedFS) checks the upload check, not an
  outage.
- **Verified.** By hand: with an unreachable store, the job failed with the
  store's error after its attempts, and the worker logged it once per
  attempt. Not by a committed test.

## The server's disk is lost

- **What happens.** Job state is restored from the nightly backup
  (`deploy/README.md`). Results and matrices are lost, an accepted risk
  (ADR 0019).
- **Evidence.** CI's `compose` job takes a backup and restores it into a
  scratch database.
- **Verified.** The restore, by CI. A full rebuild of a server from a backup,
  not.
