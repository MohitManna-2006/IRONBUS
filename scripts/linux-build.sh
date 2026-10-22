#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
image="ironbus-linux-dev"
docker build --platform linux/amd64 -t "$image" -f "$root/Dockerfile" "$root"
docker run --rm --platform linux/amd64 -v "$root:/work" -w /work "$image" bash -lc 'cmake -S . -B build-linux-x86_64 -DCMAKE_BUILD_TYPE=Release -G Ninja && cmake --build build-linux-x86_64 -j2 && ctest --test-dir build-linux-x86_64 --output-on-failure'
