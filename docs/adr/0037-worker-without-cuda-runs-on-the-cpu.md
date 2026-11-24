# 0037. A worker built without CUDA runs on the CPU

- **Status:** Accepted; the scheduler tells the kinds apart since 0039
- **Date:** 2026-11-24
- **Related:** 0011, 0017, 0018, 0023, 0024, 0032

## Context

Some machines that could take part have no NVIDIA GPU. Measured with
`vrp-bench --device cpu` (see `bench/README.md`), a 24-thread CPU computes
distances faster than the GPU, and takes 1.4 to 2.1 times as long per solver
step. Such a machine is worth adding as a worker.

## Decision

Built without CUDA (the `worker-cpu` preset), the worker runs every task on
the CPU, using `vrp-core`'s reference code: DISTANCE runs one Dijkstra per
row on every thread, and SOLVE runs the same island with
`reference::LocalSearch` on a thread pool. With the same seed, the island
finds the same solutions as on the GPU. The protocol does not change. The
worker registers as GPU `CPU (N threads)`, with the machine's memory as GPU
memory, which the scheduler checks problems against. It has no NVML metrics.

## Alternatives considered

- **Falling back to the CPU in the GPU build:** a worker whose GPU failed
  would carry on, slower, instead of restarting (0032).
- **A worker kind in the protocol:** only needed to schedule CPU and GPU
  workers differently, which is left for later.

## Consequences

- Gain: any machine with the worker's dependencies can join, and CI builds
  and tests the worker's CPU paths without a GPU.
- Cost: two builds of the worker. The scheduler treats CPU and GPU workers
  alike, so in a mixed pool a CPU worker's islands improve more slowly. A
  result's worker list shows which device each worker used.
- Revisit: when CPU and GPU workers share jobs. Then the protocol would carry
  a worker kind, DISTANCE would go to CPUs and SOLVE to GPUs, and the memory
  check would know the difference.

## Confirmation

The CI worker job builds and tests `worker-cpu`. `IslandTest.SearchesAlikeOnTheCpu`
shows that the CPU island equals the GPU island. A CPU worker has not yet
run a job against the full stack.
