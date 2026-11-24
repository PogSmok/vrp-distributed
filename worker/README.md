# vrp-worker

Runs the orchestrator's tasks on one GPU, or on the CPU when built without
CUDA (ADR 0024, 0037; see [On the CPU](#on-the-cpu)): PREPARE, DISTANCE, SOLVE
and FINALIZE, on top of `vrp-core`. `docs/formats/protocol.md` describes the
protocol it speaks.

## Contents

| Header           | What it is                                                                 | ADR              |
|------------------|----------------------------------------------------------------------------|------------------|
| `session_client` | The session thread: the stream, heartbeats, reconnects, log forwarding     | 0002, 0014, 0024 |
| `session_state`  | The protocol's rules, with no I/O: held attempts, kept reports, stale ones | 0013, 0022       |
| `task_runner`    | The compute thread: runs one task at a time, in short steps                | 0018, 0024, 0025 |
| `queues`         | The two queues, the only thing the two threads share                       | 0024             |
| `plan_router`    | Routes each new best plan on the roads for the live map, at low priority   | 0030             |
| `threaded_for`   | CPU threads for FINALIZE and the CPU worker, handing exceptions back       | 0024, 0025, 0037 |
| `transfer`       | Presigned GETs and PUTs through libcurl, with retries and fresh URLs       | 0009, 0014       |
| `graph_cache`    | Graph files on disk, checked against the assignment's SHA-256              | 0007, 0015       |
| `telemetry`      | JSON-line logs, and `/metrics` for Prometheus                              | 0010             |
| `gpu_monitor`    | GPU use, memory, temperature and power from NVML; none on the CPU          | 0010             |
| `config`         | The configuration file                                                     | 0019             |

## Running it

```sh
vrp-worker --config worker.json
```

```json
{
  "orchestrator": "server.lab:50051",
  "token": "<the worker's token>",
  "ca_certificate": "ca.pem",
  "metrics_listen": "0.0.0.0:9101",
  "metrics_address": "lab-pc-1:9101",
  "cache_dir": "cache",
  "log_dir": "logs"
}
```

Relative paths are relative to the file. `VRP_WORKER_TOKEN` replaces `token`,
so that the secret can stay out of the file. `metrics_address` is where
Prometheus reaches this worker; the orchestrator lists it for discovery.
Without TLS (`"insecure": true`, and no `ca_certificate`) the worker runs only
for local development, and says so at start-up (ADR 0009).

It exits with an error if the orchestrator refuses it (a wrong protocol
version or token), if a task stops making progress for 60 s, if it runs out
of memory, or after a GPU error that only a restart clears, and expects its
service manager to restart it (ADR 0019, 0024).
SIGTERM or Ctrl+C fails the running task, so that it is reassigned at once,
and stops the worker.

## How it works

- **Two threads.** The session thread owns the stream, and the compute thread
  owns the GPU or CPU work and the resident graph. Each only reads what the
  other puts in a queue, so a long step never delays a heartbeat.
- **Reports** carry an increasing `sequence`, and the latest of each attempt
  is kept until it is confirmed, then sent again after a reconnect. A newer
  solve report takes over the points and islands of the unconfirmed one it
  replaces, so nothing is lost (ADR 0022). A failure is kept the same way,
  until the `Cancel` that answers it.
- **Lost attempts.** The orchestrator cancels an attempt it has failed, and any
  that a new session lists but no longer holds. That is what ends a solve task
  whose lease ran out, and frees a report that will never be confirmed.
- **Transfers.** An unavailable store is retried with backoff, and the reason
  logged, for up to 10 minutes; then the attempt fails. A 403 means an expired
  URL: the worker asks for fresh ones. Any other refusal fails the task
  (ADR 0014).
- **Corrupt matrix chunks.** Each chunk is checked against its SHA-256, and
  read again once before it counts as corrupt. A corrupt chunk fails the job
  with that reason, since no retry can help (ADR 0014).
- **Reconnecting.** gRPC's channel owns the backoff, at most 5 s, so that a
  worker is back well within its 30 s lease. A new stream is tried every
  second, and a lasting error, such as a bad certificate, is logged once.
- **Solving.** Each island takes turns at one step. Every 2 s it exports its
  best solution if that improved, and asks the pool for another island's;
  every 3 s the task reports what improved. A migrant is checked against the
  island's own problem and matrix before it is used (ADR 0004).
- **Stall detection.** The compute thread shows progress between steps. If it
  does not for 60 s, the session thread fails the task and the worker exits.

Known limits, to change when they matter:

- Islands step in turn rather than in one batch (see `core/README.md`).
- Parsing a graph reports no progress, so a graph that takes over 60 s to
  parse would trip the stall check. A synthetic graph of 5 million nodes and
  20 million edges (300 MB) downloaded and loaded in 27 s over a 100 Mbps
  LAN, and in 3 s from the same machine.
- A graph file is held in memory while it is downloaded and checked, and the
  cache never evicts a graph version (ADR 0015's retention is still open).
- `worker.log` is not rotated.
- The session client and the plan router's thread have no automated tests;
  they were checked with real processes, by hand.

## Build and test

Each release has the worker built for Linux and Windows, with CUDA and for
the CPU (ADR 0040; `deploy/README.md`), so building is only needed for
development.

The worker needs CUDA (see `core/README.md`), except on the CPU, and
[vcpkg](https://vcpkg.io), which builds gRPC, Protobuf, libcurl, OpenSSL and
prometheus-cpp at the baseline pinned in `vcpkg.json`, in release mode only.
The first configure takes up to an hour.

```sh
export VCPKG_ROOT=<your vcpkg clone>
cmake --preset worker
cmake --build --preset worker
ctest --preset worker
```

On Windows, from a Visual Studio 2022 x64 prompt:

```bat
rem vcvars points VCPKG_ROOT at Visual Studio's own vcpkg: set it afterwards.
set VCPKG_ROOT=C:\vcpkg
rem With several Visual Studio versions, vcpkg would pick the newest, but
rem CUDA 12 needs 2022's compiler.
set "VCPKG_VISUAL_STUDIO_PATH=C:\Program Files\Microsoft Visual Studio\2022\Community"
cmake --preset worker
cmake --build --preset worker
ctest --preset worker
```

If git reports "dubious ownership" of the vcpkg clone, because another user
created it, run `git config --global --add safe.directory C:/vcpkg`.

### On the CPU

For a machine with no NVIDIA GPU, the `worker-cpu` preset builds the worker
without CUDA. It needs only vcpkg and a C++20 compiler; on Windows, the same
prompt and variables as above.

```sh
cmake --preset worker-cpu
cmake --build --preset worker-cpu
ctest --preset worker-cpu
```

It runs the same tasks with `vrp-core`'s CPU code on every thread, and finds
the same solutions as a GPU would with the same seed, more slowly (see
`bench/README.md`). It registers as a CPU named `CPU (N threads)`, with the
machine's memory and thread count, and has no GPU metrics. A job can run on
both kinds at once: the orchestrator gives SOLVE to GPU workers first and the
other phases to CPU workers first (ADR 0037, 0039).
