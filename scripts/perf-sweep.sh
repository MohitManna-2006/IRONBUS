#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="${IRONBUS_BENCH_BIN:-${root}/build/ironbus_bench}"

usage() {
  echo "Usage: $0 OUTPUT_DIR COUNT CAPACITY PAYLOAD REPETITIONS FANOUTS [PUB_STRATEGY] [SUB_STRATEGY] [SPIN_COUNT] [PUB_CPU|none] [SUB_CPU|none] [NUMA_NODE|none]" >&2
  echo "FANOUTS is a comma-separated list; NUMA_NODE uses numactl CPU and memory binding." >&2
}

if [[ $# -lt 6 || $# -gt 12 ]]; then
  usage
  exit 2
fi

output_dir="$1"
count="$2"
capacity="$3"
payload="$4"
repetitions="$5"
fanouts="$6"
publisher_strategy="${7:-adaptive}"
subscriber_strategy="${8:-adaptive}"
spin_count="${9:-1000}"
publisher_cpu="${10:-none}"
subscriber_cpu="${11:-none}"
numa_node="${12:-none}"

if [[ ! -x "${binary}" ]]; then
  echo "Benchmark binary is not executable: ${binary}" >&2
  echo "Build with: cmake --build build --target ironbus_bench" >&2
  exit 1
fi
if ! [[ "${repetitions}" =~ ^[1-9][0-9]*$ ]]; then
  echo "REPETITIONS must be a positive integer" >&2
  exit 2
fi
if [[ "${numa_node}" != none ]] && ! command -v numactl >/dev/null 2>&1; then
  echo "NUMA binding requested but numactl is not installed" >&2
  exit 1
fi

IFS=',' read -r -a fanout_values <<< "${fanouts}"
if [[ ${#fanout_values[@]} -eq 0 ]]; then
  usage
  exit 2
fi
for fanout in "${fanout_values[@]}"; do
  if ! [[ "${fanout}" =~ ^[1-9][0-9]*$ ]] || (( fanout > 256 )); then
    echo "Each fanout must be an integer from 1 to 256" >&2
    exit 2
  fi
done

mkdir -p "${output_dir}"
output_dir="$(cd "${output_dir}" && pwd)"
if [[ -e "${output_dir}/metadata.txt" || -e "${output_dir}/results.tsv" ]]; then
  echo "Output directory already contains a performance run: ${output_dir}" >&2
  exit 1
fi

{
  echo "utc_started=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "host=$(hostname)"
  echo "git_commit=$(git -C "${root}" rev-parse HEAD 2>/dev/null || echo unknown)"
  echo "git_dirty=$(if [[ -n "$(git -C "${root}" status --porcelain 2>/dev/null || true)" ]]; then echo true; else echo false; fi)"
  echo "uname=$(uname -a)"
  echo "benchmark_binary=${binary}"
  echo "count=${count} capacity=${capacity} payload=${payload} repetitions=${repetitions} fanouts=${fanouts}"
  echo "publisher_strategy=${publisher_strategy} subscriber_strategy=${subscriber_strategy} spin_count=${spin_count}"
  echo "publisher_cpu=${publisher_cpu} subscriber_cpu=${subscriber_cpu} numa_node=${numa_node}"
  echo
  echo "--- current affinity ---"
  taskset -pc $$ 2>&1 || true
  echo
  echo "--- CPU and NUMA topology ---"
  if command -v lscpu >/dev/null 2>&1; then lscpu; else echo "lscpu unavailable"; fi
  if command -v numactl >/dev/null 2>&1; then numactl -H; else echo "numactl unavailable"; fi
  echo
  echo "--- CPU governor settings ---"
  shopt -s nullglob
  governors=(/sys/devices/system/cpu/cpu*/cpufreq/scaling_governor)
  if ((${#governors[@]})); then
    for governor in "${governors[@]}"; do echo "${governor}=$(cat "${governor}")"; done
  else
    echo "governor data unavailable"
  fi
  echo
  echo "--- cgroup CPU constraints ---"
  for f in /sys/fs/cgroup/cpu.max /sys/fs/cgroup/cpuset.cpus.effective; do
    if [[ -r "${f}" ]]; then echo "${f}=$(cat "${f}")"; fi
  done
} > "${output_dir}/metadata.txt"

printf 'fanout\trepetition\tmessages_per_second\tp50_ns\tp99_ns\tp999_ns\tcombined_cpu_seconds\tlog\n' \
  > "${output_dir}/results.tsv"

for fanout in "${fanout_values[@]}"; do
  for (( repetition = 1; repetition <= repetitions; ++repetition )); do
    log="${output_dir}/fanout-${fanout}-run-${repetition}.txt"
    args=("${count}" "${capacity}" "${payload}" "${publisher_strategy}" "${subscriber_strategy}"
      "${spin_count}" "${publisher_cpu}" "${subscriber_cpu}" "${fanout}")
    if [[ "${numa_node}" == none ]]; then
      "${binary}" "${args[@]}" | tee "${log}"
    else
      numactl --cpunodebind="${numa_node}" --membind="${numa_node}" \
        "${binary}" "${args[@]}" | tee "${log}"
    fi
    awk -v fanout="${fanout}" -v repetition="${repetition}" -v logfile="$(basename "${log}")" '
      { for (i = 1; i <= NF; ++i) { split($i, part, "=");
          if (part[1] == "messages_per_second") throughput = part[2];
          else if (part[1] == "publisher_to_ack_observed_ns_p50") p50 = part[2];
          else if (part[1] == "p99") p99 = part[2];
          else if (part[1] == "p999") p999 = part[2];
          else if (part[1] == "combined_cpu_seconds") cpu = part[2]; }
        if (throughput == "" || p50 == "" || p99 == "" || p999 == "" || cpu == "") exit 1;
        printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n", fanout, repetition, throughput, p50, p99, p999, cpu, logfile;
      }' "${log}" >> "${output_dir}/results.tsv"
  done
done

echo "Results written to ${output_dir}/metadata.txt and ${output_dir}/results.tsv"
