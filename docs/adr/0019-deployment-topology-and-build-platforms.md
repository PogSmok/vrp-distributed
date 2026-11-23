# 0019. Deployment: third-party services in Docker Compose, our programs native

- **Status:** Accepted; MinIO replaced by SeaweedFS in 0029
- **Date:** 2026-11-21
- **Related:** 0008, 0009, 0010, 0023, 0024, 0026

## Context

The lab has one server and 1 to 8 GPU workers on Windows and Linux. The
server runs four services we do not write (PostgreSQL, MinIO, Prometheus,
Grafana) and the orchestrator. Workers must restart on their own (0024), run
natively so benchmarks are fair, and build from one source tree. TLS must
work without a public certificate authority.

## Decision

- **Third-party services in Docker Compose.** PostgreSQL, MinIO, Prometheus
  and Grafana run from one Compose file in the repository, with their data
  in volumes. Docker is used here because:
  - each of these services installs and is configured differently on
    Windows and on Linux, and Compose makes the setup the same on either
    server;
  - their versions and configuration are pinned in the repository, so the
    lab cannot drift;
  - every developer machine and the CI integration tests start the same
    stack with one command.

  Linux is the preferred server host; Windows with Docker Desktop also
  works.
- **Our programs run natively.** None of the benefits above applies to code
  we build ourselves, and a container would add an image to build for each
  change.
  - The orchestrator runs on the server as a service (systemd, or a Windows
    service through WinSW), from a virtual environment created from a lock
    file.
  - Each worker runs as the same kind of service on its own machine. Its
    configuration holds the orchestrator address, its token, the CA
    certificate path and its metrics port.
  - Workers are upgraded by hand, and the orchestrator refuses a mismatched
    protocol version (0008).
- **Network.** The firewall opens the orchestrator, object store and Grafana
  ports on the server to the lab network only. Workers accept connections
  only on their metrics port, and only from the server.
- **Build.**
  - CMake, with MSVC on Windows and GCC on Linux. Every dependency is
    pinned: GoogleTest by URL and hash through FetchContent, and the
    worker's gRPC, Protobuf and libcurl through vcpkg on a pinned baseline.
  - CUDA pinned to 12.x, for GPUs of compute capability 6.0 or newer (CUDA
    13 drops GPUs older than 7.5). The binary carries native code for each
    architecture plus PTX, and every binary is stamped with its git
    version.
  - CI builds both platforms and runs the CPU tests; GPU tests run on a lab
    machine.
  - Python tools install from a lock file.
- **Operations.**
  - A script creates a private lab CA and certificates valid for one year.
  - The orchestrator migrates the schema at start-up, and refuses to start
    on a schema newer than it knows.
  - A nightly `pg_dump` is copied to the admin workstation. Losing the
    server's other data is an accepted risk.

## Alternatives considered

- **Installing the four services directly on the server:** a different
  manual setup on each operating system, versions drift, and developers
  cannot run the same stack.
- **The orchestrator in Compose too:** a single start command, but an image
  to build for every change, for no other benefit.
- **Workers in containers:** on Windows, that means Linux inside WSL2.
- **Linux workers in WSL2 on Windows machines:** fragile networking and
  autostart; this is the fallback if the native Windows build costs too
  much.
- **Kubernetes:** disproportionate for one server.

## Consequences

- Gain: a repeatable server and development stack, native performance, and
  identical dependencies on both platforms.
- Cost: Docker on the server and on developer machines; two build
  platforms and two service setups; manual upgrades; the CA certificate on
  every machine.
- Revisit: whether CUDA works inside a Windows service on consumer GPUs (if
  not, start the worker as an automatic task in the user's session);
  unpinning CUDA once the oldest GPU allows it.

## Confirmation

- The four services come up from scratch with one command, on the server
  and on a developer machine.
- Fresh Windows and Linux workers run a job, and restart after being
  killed.
- The schema is created on an empty database, and a newer schema is
  refused.
- The nightly backup restores into a fresh PostgreSQL.
