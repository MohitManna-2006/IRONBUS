# Phase 5: Performance engineering

## Measurement contract

The benchmark reports serial broadcast retirement cadence: the publisher sends one message, waits until every registered subscriber ACKs it, and only then sends the next. Its latency is publisher-observed publish-to-all-ACK completion time, including endpoint work, kernel wakeups, process scheduling, and the selected wait strategy. It does not measure isolated atomic cost, batched ring throughput, or network latency.

Use `scripts/perf-sweep.sh` to repeat a fixed workload over a comma-separated fanout list. It preserves every raw benchmark line, writes a tab-separated summary, and captures the source revision, dirty-tree state, kernel, CPU topology, current affinity, governor settings, cgroup limits, and available NUMA topology. Optional CPU indices pin the publisher and all subscribers as supported by `ironbus_bench`. An optional NUMA node uses `numactl` to bind the benchmark process and inherited workers to that node.

Example on a native Linux x86-64 machine:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
scripts/perf-sweep.sh perf-results/run-001 100000 1024 64 5 1,2,4,8 adaptive adaptive 1000 none none none
```

For controlled placement, select CPUs allowed by the current cpuset and optionally a NUMA node:

```bash
scripts/perf-sweep.sh perf-results/node0 100000 1024 64 5 1,2,4,8 futex futex 0 2 3 0
```

Run sweeps on an otherwise idle native host. Record the CPU model, SMT state, BIOS power settings, kernel, CPU governor, cpuset, NUMA topology, background load, and whether the run is bare metal or virtualized. Keep the `metadata.txt`, `results.tsv`, and per-run log files together. Repeat a baseline before and after any code change using the same binary build flags and placement. Compare repetitions and tail latency distributions; do not draw conclusions from a single run. Docker Desktop or emulated x86-64 output is useful for checking the harness but is not representative performance evidence.

## Current implementation and limits

The transport already separates publication, retention, data-wakeup, and space-wakeup state onto cache-line-aligned regions and supports publisher/subscriber CPU affinity in the harness. The sweep runner adds reproducible repeated fanout observations and optional NUMA binding. No cache-line layout or wake policy has been changed in this phase because there is no native Linux baseline in the current macOS/Docker-emulated environment from which to justify a performance tradeoff.

Phase 5 is not accepted until repeated native Linux x86-64 baselines are recorded and a bottleneck is measured. Then make one targeted optimization at a time, compare against the saved baseline with the same workload and placement, and preserve both result sets. Per-subscriber rings, wake relays, cache-line reshaping, or other architectural changes require evidence that their costs improve the stated broadcast workload without weakening delivery or recovery invariants.
