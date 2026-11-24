# 0039. CPU and GPU workers share jobs; each phase prefers its kind

- **Status:** Accepted
- **Date:** 2026-11-24
- **Related:** 0007, 0011, 0013, 0017, 0037

## Context

Under 0037 the scheduler could not tell a CPU worker from a GPU worker: it
checked a CPU worker's RAM against the GPU's memory needs, and gave any task
to whichever worker came first by name. Measured on one machine (an
i7-14650HX against an RTX 5050 Laptop, `bench/README.md`), the CPU computes
distances faster than the GPU at every size, and its solver steps take 1.4
to 2.1 times as long.

## Decision

- **Protocol 1.1.** `Register` carries the worker's `device_kind` and, for a
  CPU worker, its thread count. A worker that sends no kind is a GPU worker.
- **Memory.** Each worker is checked against its own kind's needs. A GPU
  worker's are unchanged. A CPU worker's RAM must hold the graph with both
  directions and weights, plus, in DISTANCE, one Dijkstra's arrays per
  thread, or, in SOLVE, the matrix and the islands. A job is refused only if
  a phase fits no connected worker, so a pool can split a job that no single
  worker fits.
- **Preference.** SOLVE goes to GPU workers first, and every other phase to
  CPU workers first; either falls back to the other kind. A solve group
  waits, within the existing group wait, for its preferred workers to become
  idle, since a GPU worker may still be finishing DISTANCE. DISTANCE tasks
  never wait: any idle worker that fits takes the next one.

## Alternatives considered

- **Each phase only on its kind:** a pool of one kind could not run a job,
  and idle workers would leave DISTANCE rows waiting.
- **Weighting tasks by measured speed:** needs benchmarks of each machine,
  which a lab does not yet have.

## Consequences

- Gain: small DISTANCE phases run on the faster CPU, and the solver's few
  tasks run on GPUs when there is a choice.
- Cost: two memory estimates to keep in step with the code they describe.
  The CPU estimate assumes about one queue entry per node in each search.
- Revisit: when benchmarks of mixed pools on separate machines show whether
  CPU islands help a GPU pool's search or only slow its migrant pool.

## Confirmation

`test_each_phase_prefers_its_kind_of_worker` and
`test_each_phase_runs_on_the_one_worker_it_fits` in the scheduler's tests.
Mixed pools have not been benchmarked on separate machines.
