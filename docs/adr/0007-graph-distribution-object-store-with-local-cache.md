# 0007. Graph distribution: object store with local cache

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** 0005, 0009, 0011, 0015

## Context

Workers need graph files on their own machines before they can load them
into GPU memory. Which part of the graph each worker holds is decided in
0011; this ADR decides how the files get there. Graph files are large and
must not travel through the orchestrator. A new worker must be able to join
without manual file copying or configuration.

## Decision drivers

- New workers join without manual setup.
- The orchestrator handles small control messages only.
- Workers must not hold object-store credentials (0009).

## Decision

The preprocessor uploads each graph version (0015) to the object store
(MinIO, S3-compatible). When the orchestrator assigns work, it includes the
graph version and a presigned download URL. The worker downloads the graph
if it is not in its local cache (verified by checksum) and loads it into GPU
memory. Workers report their GPU memory at registration, and the orchestrator
does not assign a graph that does not fit.

## Alternatives considered

### Option A: Local files copied to each machine by hand
- Pros: no extra service.
- Cons: a new worker cannot work until someone configures it.
- Why rejected: contradicts self-service worker registration.

### Option B: Distribute through the orchestrator
- Pros: no extra service.
- Cons: large transfers on the control path.
- Why rejected: the orchestrator should handle small control messages only.

### Option C: Shared network file system (SMB / NFS)
- Pros: simple semantics.
- Cons: awkward on mixed Windows and Linux machines; harder to monitor; every
  worker needs access rights to the share.
- Why rejected: object storage with presigned URLs is more portable and
  needs no credentials on workers.

## Consequences

**What we gain:**
- Workers join without manual setup; graph versions are explicit.
- Plain HTTPS download with presigned URLs, so no S3 SDK is needed in C++
  (for example, libcurl is enough).

**What we pay for it:**
- One more service to run and monitor; a fresh worker downloads the graph on
  first use.

**Risks / to revisit later:**
- Check MinIO's current licence (AGPL) and project status. The S3 API keeps
  it replaceable by compatible stores.

## Confirmation

Start a new worker with no pre-copied files and verify that it runs a job.
Corrupt a cached graph file and verify that the checksum triggers a
re-download.
