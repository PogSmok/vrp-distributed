# 0029. Object store: SeaweedFS instead of MinIO

- **Status:** Accepted; graph uploads refined by 0031
- **Date:** 2026-11-23
- **Related:** 0007, 0009, 0016, 0019

## Context

ADR 0007 put graphs, matrices and results in MinIO, run from the Compose
stack of ADR 0019. MinIO's community images are no longer published:
Docker Hub has no `minio/minio` repository, and Quay requires a login. ADR
0007 had expected this ("any S3-compatible store can replace it").

## Decision

- **Store.** SeaweedFS runs the S3 gateway from one container, pinned to
  an exact image version. The rest of ADR 0007 stands: an object store
  with a local cache on each worker.
- **Interface.** The orchestrator and the preprocessor speak plain S3
  through boto3, the standard S3 SDK, so the store can be replaced again
  without code changes. Workers use only presigned URLs (ADR 0009).
- **Expiry.** The Artifact Manager deletes results and matrices older than
  30 days itself (ADR 0016), as SeaweedFS supports S3 lifecycle rules only
  in part.

## Alternatives considered

- **RustFS:** a drop-in MinIO replacement under Apache-2.0, but young
  (2024) for the component that holds every result.
- **Garage:** light and robust, but AGPL-3.0, the licence ADR 0007 already
  flagged, and it needs a cluster layout set up before first use.
- **MinIO built from source:** possible, but an image to build and
  maintain for a service we do not write.

## Consequences

- Gain: an Apache-2.0, actively maintained store; S3 code that is not tied
  to one product.
- Cost: an expiry pass in the orchestrator; SeaweedFS's S3 gateway needs an
  identity file for its credentials.

## Confirmation

Against SeaweedFS 4.54: an upload through a presigned PUT has its MD5 as
its ETag, a presigned GET downloads it, a URL signed for one object is
refused for another, and a tampered signature is refused. CI runs the
Artifact Manager's tests against the pinned image.
