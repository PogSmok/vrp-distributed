# 0007. Graph distribution: object store with local cache

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** 0005, 0009, 0011, 0015

## Context

Workers need graph files on their own disk before loading them into GPU
memory. The files are large and must not pass through the orchestrator, and
a new worker must join without manual setup or storage credentials.

## Decision

The preprocessor uploads each graph version to MinIO (S3-compatible). An
assignment carries the graph version and a presigned download URL, and the
worker downloads the graph if its checksum-verified cache does not have it.
The orchestrator assigns a task only if the graph, the job's matrix and the
task's working memory together fit in the GPU memory the worker reported.
It rejects at submission a problem that would fit on none of the registered
workers.

## Alternatives considered

- **Copying files by hand:** a new worker cannot work until someone sets it
  up.
- **Through the orchestrator:** large transfers on the control path.
- **A network share (SMB, NFS):** awkward across Windows and Linux, and every
  worker needs access to the share.

## Consequences

- Gain: new workers need no setup; plain HTTPS downloads (libcurl, no S3
  SDK in C++).
- Cost: one more service; a fresh worker downloads the graph on first use.
- Revisit: MinIO is AGPL and its community edition is less maintained; any
  S3-compatible store can replace it.

## Confirmation

A fresh worker with no files runs a job. A corrupted cached graph is
downloaded again. A problem too large for every registered worker's GPU is
rejected at submission.
