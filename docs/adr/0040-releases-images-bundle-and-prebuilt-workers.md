# 0040. Releases: images on GHCR, a deploy bundle and prebuilt workers

- **Status:** Accepted
- **Date:** 2026-11-24
- **Related:** 0019, 0031, 0035, 0037

## Context

Running the system needed a checkout and a build: Docker built the control
plane's images on the server (0031), and each worker machine needed CUDA,
Visual Studio or GCC, and an hour of vcpkg to build gRPC.

## Decision

A tag `v<version>` runs the release workflow, which publishes nothing until
every part has built and its tests have passed:

- **Images.** The orchestrator and client images, on GitHub's container
  registry as `ghcr.io/<owner>/vrp-orchestrator:<version>` and
  `vrp-client:<version>`.
- **Deploy bundle.** The Compose file with those images instead of builds,
  and the configuration files it mounts.
- **Workers.** For Linux and Windows, with CUDA and without (0037): a
  stripped executable with its service file and third-party notices. Their
  libraries are linked in, except glibc on Linux and, on Windows, the C++
  runtime DLLs that ship beside the executable.
- **`SHA256SUMS`** of every file.

## Alternatives considered

- **Images on Docker Hub:** an account and a secret to manage, where GHCR
  works with the workflow's own token.
- **Workers in container images:** CUDA in a container needs the NVIDIA
  container toolkit on every machine, and Docker Desktop on Windows.
- **A static C++ runtime on Windows:** every vcpkg package rebuilt for it,
  for the same result as four redistributable DLLs.

## Consequences

- Gain: a lab runs a release with Docker on the server and one archive per
  worker, and every machine runs the same tagged build.
- Cost: a release takes over an hour while gRPC is not cached on the Windows
  runner. A private repository's images need a `docker login ghcr.io` with a
  token that can read packages; a public one's images, once made public.
- Revisit: if a worker machine has a glibc older than 2.38.

## Confirmation

A pre-release tag such as `v1.0.0-rc.1` runs the whole workflow. Its bundle
starts from scratch with `docker compose up --wait`, and its workers register
and solve a job.
