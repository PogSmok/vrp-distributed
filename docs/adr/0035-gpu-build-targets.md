# 0035. GPU build targets: compute capability 6.1 and 12.0

- **Status:** Accepted
- **Date:** 2026-11-24
- **Related:** 0019

## Context

ADR 0019 pinned CUDA to 12.x for "compute capability 6.0 or newer". The
oldest GPU in the lab is a GTX 1060 (6.1), the development GPU an RTX 5050
Laptop (12.0), and no GPU of 6.0 is in use.

## Decision

The build targets `61;120-real`: native code for both GPUs, and compute 6.1
PTX for those in between. That needs CUDA 12.8, the first toolkit with
architecture 120, or a later 12.x, as CUDA 13 drops 6.1. CMake refuses any
other toolkit unless `CMAKE_CUDA_ARCHITECTURES` is set.

## Alternatives considered

- **Compute 6.0 (0019):** adds a target for a GPU the lab does not have.
- **CUDA 13:** drops the GTX 1060.

## Consequences

- Gain: one binary for every worker in the lab.
- Cost: CUDA 12.x needs Visual Studio 2022 on Windows.
- Revisit: when the GTX 1060 leaves the lab.

## Confirmation

The `cuda` and `worker` CI jobs build with CUDA 12.9. GPU tests run on the
RTX 5050. Running on a GTX 1060 has not been verified.
