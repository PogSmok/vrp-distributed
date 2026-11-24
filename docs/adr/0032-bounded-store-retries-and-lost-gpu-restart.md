# 0032. Bounded store retries; a lost GPU restarts its worker

- **Status:** Accepted
- **Date:** 2026-11-24
- **Related:** 0014, 0019, 0024

## Context

Two failures left a job or a worker stuck with no reason shown. ADR 0014
retried an unreachable store without limit, so a wrong store address held a
job forever. After a GPU fault such as a driver reset, CUDA fails every call
until the process restarts, but ADR 0024 returned task errors as values, so
the worker stayed connected and failed each task it was given.

## Decision

- **Store.** A transfer logs why the store is unavailable and retries with
  backoff. Still failing after 10 minutes, it fails its attempt; the usual
  attempt limit then fails the job with that reason.
- **GPU.** The CUDA errors documented as needing a process restart become
  `kDeviceLost`. The worker reports its task as failed and exits with an
  error, and its service manager restarts it (0019).

## Alternatives considered

- **Retrying the store forever:** a job can wait with no end and no reason.
- **Failing a job on the first store error:** a short outage would lose it.
- **Excluding a failing worker in the orchestrator:** more scheduler state,
  while a restart clears the fault.

## Consequences

- Gain: every job ends with a stored reason, and a worker recovers alone.
- Cost: a store outage of about 30 minutes fails the running job.

## Confirmation

With an unreachable store, the job fails with the store's error and the
worker logs it once per attempt. With an injected CUDA launch failure, the
worker exits with an error and the job finishes on another worker.
