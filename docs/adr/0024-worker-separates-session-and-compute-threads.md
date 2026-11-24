# 0024. Worker: separate session and compute threads, short GPU steps

- **Status:** Accepted; extended by 0032
- **Date:** 2026-11-21
- **Related:** 0010, 0013, 0017, 0018, 0019, 0022, 0023

## Context

A worker must keep its lease alive during long GPU work and stop within
about a second when told to. On Windows, a kernel that runs for more than 2
seconds makes the display driver reset the GPU (TDR). Heartbeats alone
would hide a computation that is stuck.

## Decision

- **Session thread.** Owns the gRPC stream: registration, heartbeats,
  reconnects, fresh URLs, resending unconfirmed reports, and forwarding log
  lines (0010).
- **Compute thread.** Owns the CUDA context and the resident graph, and runs
  one task at a time. It works in steps well under a second, sized by 0017,
  0018 and 0025. Between steps it checks for cancellation and exchanges
  migrants.
- **Communication.** The two threads talk only through two queues.
- **Stall detection.** Heartbeats carry the time of the last progress.
  After a stall timeout, the worker reports its task as failed and exits,
  and its service manager restarts it (0019).
- **Out of memory.** Running out of host memory is fatal in the same way:
  the worker logs it and exits, and is restarted. Recovery treats it like
  any lost worker (0014). Task errors, by contrast, are returned as values
  and reported (0013).

## Alternatives considered

- **One thread for everything:** a slow step delays heartbeats.
- **A process per task:** reloads the graph every time.
- **Several tasks at once on one GPU:** breaks one task per GPU and clean
  benchmarks.

## Consequences

- Gain: leases reflect real progress; prompt cancellation; no TDR resets.
- Cost: bounded kernels may cost some efficiency; two threads and two
  queues to get right.
- Revisit: the step size per GPU, measured on the slowest card.

## Confirmation

On the slowest GPU, under Windows: no step takes more than 1 second,
heartbeats keep their interval, and a cancel takes effect within a second.
A frozen compute thread is reported after the stall timeout.
