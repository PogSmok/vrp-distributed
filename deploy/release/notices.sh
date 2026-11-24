#!/bin/sh
# Prints a worker build's third-party notices (ADR 0040): every vcpkg package
# it links, nlohmann/json, and what else ships with it.
#
#   notices.sh <vcpkg_installed/<triplet>> <nlohmann_json-src> [cuda] [msvc]
set -eu
installed=$1
json=$2
shift 2
echo "vrp-worker includes the following third-party software."
for copyright in "$installed"/share/*/copyright; do
    port=$(basename "$(dirname "$copyright")")
    case $port in vcpkg-*) continue ;; esac  # build helpers, not linked
    printf '\n==== %s ====\n\n' "$port"
    cat "$copyright"
done
printf '\n==== nlohmann/json ====\n\n'
cat "$json/LICENSE.MIT"
for extra in "$@"; do
    case $extra in
    cuda) printf '\n==== NVIDIA CUDA runtime ====\n\n%s\n' \
        "The CUDA runtime (cudart) is linked in under the NVIDIA CUDA Toolkit End User License Agreement: https://docs.nvidia.com/cuda/eula/" ;;
    msvc) printf '\n==== Microsoft Visual C++ runtime ====\n\n%s\n' \
        "msvcp140*.dll and vcruntime140*.dll are redistributed under the Microsoft Visual Studio license terms: https://learn.microsoft.com/cpp/windows/redistributing-visual-cpp-files" ;;
    *) echo "unknown notice: $extra" >&2; exit 2 ;;
    esac
done
