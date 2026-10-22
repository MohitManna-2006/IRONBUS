#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="${root}/build/ironbus_bench"
count="${1:-100000}"
capacity="${2:-1024}"
payload="${3:-64}"
spin_count="${4:-1000}"
subscriber_count="${5:-1}"
if [[ ! -x "${binary}" ]]; then
  echo "Build first: cmake --build build --target ironbus_bench" >&2
  exit 1
fi
for strategy in busy futex adaptive; do
  echo "strategy=${strategy}"
  /usr/bin/time -v "${binary}" "${count}" "${capacity}" "${payload}" \
    "${strategy}" "${strategy}" "${spin_count}" none none "${subscriber_count}"
done
