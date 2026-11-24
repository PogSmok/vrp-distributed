# Setup: from a clean machine to a solved job

This guide sets up one machine as both the server and a worker, then solves
400 customers around Warsaw. `deploy/README.md` covers a lab of several
machines.

You need:

- an NVIDIA GPU of compute capability 6.1 (GTX 10 series) or newer, with a
  driver that supports CUDA 12.9, or else the CPU worker (step 3);
- an internet connection for the downloads, including a 300 MB map extract.

The Windows path below was followed on the development machine; the Linux
path has not yet been tried on a clean machine.

## 1. Install the tools

| Tool         | Version              | Used for                                       |
|--------------|----------------------|------------------------------------------------|
| Git          | any recent           | this repository and vcpkg                      |
| Docker       | 26 or newer          | the control plane (Docker Desktop on Windows)  |
| uv           | 0.13.7 or newer      | Python, the preprocessor and the demo script   |
| CUDA toolkit | 12.8 or a later 12.x | the GPU code (CUDA 13 drops the GTX 10 series) |
| C++ compiler | MSVC 2022 or GCC 13  | the worker                                     |
| CMake, Ninja | CMake 3.25 or newer  | the build                                      |
| vcpkg        | a full clone         | gRPC, Protobuf, libcurl and prometheus-cpp     |

### Windows 11

1. Install [Git](https://git-scm.com/download/win) and
   [Docker Desktop](https://docs.docker.com/desktop/setup/install/windows-install/),
   and start Docker Desktop.
2. Install [Visual Studio 2022](https://visualstudio.microsoft.com/vs/older-downloads/)
   with the **Desktop development with C++** workload. It brings MSVC, CMake
   and Ninja; a separately installed CMake 3.25 or newer works too, and is
   what the run-through of this guide used. CUDA 12 does not support Visual
   Studio 2026.
3. Install [CUDA 12.9](https://developer.nvidia.com/cuda-12-9-0-download-archive)
   after Visual Studio, so that it adds its build integration.
4. Install uv, in PowerShell:

   ```powershell
   powershell -ExecutionPolicy ByPass -c "irm https://astral.sh/uv/install.ps1 | iex"
   ```

5. Clone and bootstrap vcpkg, in a short path such as `C:\vcpkg`:

   ```bat
   git clone https://github.com/microsoft/vcpkg C:\vcpkg
   C:\vcpkg\bootstrap-vcpkg.bat -disableMetrics
   ```

### Ubuntu 24.04

```sh
sudo apt-get update
sudo apt-get install -y git build-essential cmake ninja-build pkg-config curl zip unzip tar
# Docker Engine: https://docs.docker.com/engine/install/ubuntu/
# CUDA 12.9:     https://developer.nvidia.com/cuda-12-9-0-download-archive (deb, network)
curl -LsSf https://astral.sh/uv/install.sh | sh
git clone https://github.com/microsoft/vcpkg ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh -disableMetrics
```

Put CUDA's `bin` directory on `PATH`, usually `/usr/local/cuda-12.9/bin`.

## 2. Get the code

```sh
git clone <this repository's URL> vrp-distributed
cd vrp-distributed
```

The remaining commands run from this directory.

## 3. Build the worker

The first configure builds gRPC and the other dependencies with vcpkg, which
takes up to an hour; later builds take a minute or two.

On Windows, open **x64 Native Tools Command Prompt for VS 2022** from the
Start menu, go to the repository, and run:

```bat
set VCPKG_ROOT=C:\vcpkg
set "VCPKG_VISUAL_STUDIO_PATH=C:\Program Files\Microsoft Visual Studio\2022\Community"
cmake --preset worker
cmake --build --preset worker
ctest --preset worker
```

On Linux:

```sh
export VCPKG_ROOT=~/vcpkg
cmake --preset worker
cmake --build --preset worker
ctest --preset worker
```

`ctest` includes the GPU tests, which need the GPU. If CMake finds the wrong
CUDA toolkit, such as CUDA 13 installed beside 12.9, add
`-DCMAKE_CUDA_COMPILER=<CUDA 12.9>/bin/nvcc` and
`-DCUDAToolkit_ROOT=<CUDA 12.9>` to the first `cmake` command. The worker is
`build/worker/worker/vrp-worker`, with `.exe` on Windows.

Without an NVIDIA GPU, skip CUDA and use the `worker-cpu` preset in place of
`worker` in the three commands. That worker runs every task on the CPU, slower
in SOLVE (ADR 0037), and is `build/worker-cpu/worker/vrp-worker`.

## 4. Start the demo

In a terminal of your choice, from the repository:

```sh
uv run --no-project deploy/demo.py
```

It starts the control plane in Docker Compose, downloads the Mazowieckie
extract from Geofabrik, imports it as a road graph (a few minutes), registers
it, and writes a worker configuration to `data/demo/worker/`. It ends by
printing the addresses and the worker's command. Run it again at any time:
it reuses what it has already made. With an extract you already have, pass
`--extract <file.osm.pbf>`.

## 5. Start the worker

In a second terminal, from the repository, run the command the demo printed:

```sh
build/worker/worker/vrp-worker --config data/demo/worker/worker.json
```

On Windows: `build\worker\worker\vrp-worker.exe --config data\demo\worker\worker.json`.
The CPU worker is in `build/worker-cpu` instead. It logs `registered` when it has connected. Leave it running.

## 6. Solve a job

1. Open <https://localhost:8501>, accept the certificate warning (or trust
   `data/demo/worker/ca.pem`), and sign in with the token the demo printed,
   which is also in `data/demo/client-token`.
2. Choose **Build a problem**. It starts with 400 customers around Warsaw.
3. Press **Solve this problem**, then **Start solving**.
4. The map follows the job: snapping, road distances, then the search, with
   the best plan drawn along the roads. With the default 60 s time limit, the
   job ends about a minute later with the final plan, which **Export result**
   downloads.

Grafana is at <http://localhost:3000>, user `admin`, with the password from
`docker compose -f deploy/compose.yaml run --rm admin grafana-password`.

## Stop and start again

```sh
docker compose -f deploy/compose.yaml stop
docker compose -f deploy/compose.yaml start
```

Stop the worker with Ctrl+C. `docker compose down` removes the containers but
keeps the data; adding `-v` also deletes the secrets, which invalidates the
worker's configuration until the demo makes a new one.
