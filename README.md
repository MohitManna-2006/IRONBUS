# IRONBUS

**No sockets on the hot path. No silent overwrites. ACK before reuse.**

A Linux x86-64, C++20, cross-process shared-memory messaging system.

> **Status: Phases 0–4 complete; Phase 5 pending.** Phase 4 failure-recovery acceptance passed on native Linux x86-64 and its bounded acceptance checks are integrated into CI. Phase 5 remains pending controlled native performance baselines. Broker discovery and zero-copy payload views are not implemented.

## Why this exists

A market-data process needs to distribute normalized events to a separate trading process with minimal transport overhead. A slow or dead consumer must not cause silent message corruption. IRONBUS is a focused systems-engineering laboratory for process-shared atomics, cache coherence, memory ordering, backpressure, failure recovery, and measurable latency.

## Architecture today

```text
       publisher process                 subscriber processes
       ------------------                 --------------------
       claim exclusive OFD lock          claim stable-ID member locks
               |                           |        |        |
               v                           v        v        v
       write Slot[N] payload            read N   read N   read N
       release-store published          ACK[i]   ACK[j]   ACK[k]
               |                          \       |       /
               +------ POSIX shared memory ----+--+
                       bounded broadcast ring

       reuse Slot[N] only when min(ACK[registered readers]) >= N
```

**Design choices:** fixed-size 256-byte slots, up to 240 payload bytes, power-of-two ring capacity, no dynamic allocation in the library's publish/receive operations, independent 64-byte-aligned atomic cursors, exclusive publisher and per-subscriber roles enforced with Linux OFD locks, and exact ABI validation upon attach. This is broadcast SPMC: all subscribers receive each message, and the slowest registered subscriber controls reuse. Adding or explicitly removing identities requires a quiescent publisher; stale members are never evicted automatically.

This is **not** an order book, exchange, matching engine, or network feed handler. It complements AegisFeed and MarketTwin instead of duplicating them.

## Prerequisites

- Linux x86-64 with a C++20 compiler (GCC 11+ or recent Clang), CMake 3.20+, and POSIX shared memory.
- macOS development can use Docker with `./scripts/linux-build.sh`; it targets Linux x86-64 (Docker Desktop uses emulation on Apple Silicon). GitHub Actions also builds on Linux x86-64.

## Build / run (Linux)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure

# Development sanitizer build
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DIRONBUS_ENABLE_SANITIZERS=ON
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure

./build/ironbus create --name demo --capacity 1024 --max-subscribers 16
# Terminal 1
./build/ironbus sub --name demo --id strategy-a --count 100000
# Terminal 2
./build/ironbus pub --name demo --count 100000
# Another subscriber process can join the same broadcast:
./build/ironbus sub --name demo --id strategy-b --count 100000
# Later
./build/ironbus stats --name demo
./build/ironbus inspect --name demo
./build/ironbus unlink --name demo
```

`inspect` is read-only. It reports each registered subscriber's ACK cursor and whether its OFD role lock is held; an inactive lock does not remove the subscriber or release its retained backlog. Development fault injection is available only when a process is explicitly launched with `IRONBUS_FAULT_POINT` set to a named transition (`create_after_open`, `create_after_size`, `create_before_ready`, `create_after_ready`, `member_initializing`, `member_registered`, `publish_before_commit`, `publish_after_commit`, `ack_before_commit`, or `ack_after_commit`). By default the process exits with status 86; set `IRONBUS_FAULT_ACTION=pause` to stop it at that point for inspection. These environment variables are intended for controlled development experiments.

The executable communicates **between actual processes** (not threads in a single executable). Register initial subscriber identities before first publication; first publish seals the initial roster. Without any subscriber, publish returns `no_subscribers` without advancing the sequence. A stalled reader retains its OFD role lock until it exits or closes its handle, and its persistent ACK cursor continues to gate ring reuse after a crash. Restart with the same `--id` to resume; use `ironbus forget --name demo --id strategy-a` only when publisher and target subscriber are stopped, understanding that this discards its backlog. No messages are silently dropped automatically.

### Benchmark

```bash
./build/ironbus_bench 1000000 4096 64
```

The harness launches one publisher and a configurable fanout of independent subscriber processes. It reports messages per second and **publisher-observed publish-to-all-ACK completion latency** (p50, p99, p99.9) using `steady_clock`, plus combined user and system CPU time from `getrusage`. Timing includes scheduler and polling effects. Run under `/usr/bin/time -v` when per-process CPU accounting is useful. These are not single-operation ring overhead or NIC wire-to-wire latencies. Record architecture, kernel, load, affinity, and multiple runs before drawing conclusions. Docker measurements are not representative of bare-metal latency.

The benchmark accepts endpoint wait policies, a bounded spin count, and optional Linux CPU indices (`none` leaves scheduling unchanged):

```bash
./build/ironbus_bench 100000 1024 64 futex adaptive 1000 none none 8
./scripts/bench-strategies.sh 100000 1024 64 1000 8
```

The comparison script runs the same workload under `/usr/bin/time -v`; it does not control system load or pin endpoints. For explicit pinning, pass publisher and subscriber CPU indices as the final two benchmark arguments. Each process reports combined user and system CPU time from `getrusage`.

For repeated fanout measurements, use `scripts/perf-sweep.sh`. It saves raw runs and records CPU, kernel, affinity, cgroup, governor, and NUMA context. See [docs/PHASE5_PERFORMANCE.md](docs/PHASE5_PERFORMANCE.md) for methodology and limits; Docker-emulated x86-64 runs check the harness but are not representative performance evidence.

Phase 2 wait-strategy samples are recorded in [docs/PHASE2_BENCHMARK_NOTES.md](docs/PHASE2_BENCHMARK_NOTES.md). Phase 3 fanout samples and interpretation are in [docs/PHASE3_BENCHMARK_NOTES.md](docs/PHASE3_BENCHMARK_NOTES.md); both are container-emulated observations, not bare-metal performance claims.

The CLI accepts `--strategy busy`, `--strategy futex`, or `--strategy adaptive` on either endpoint. Adaptive mode uses `--spin-count` predicate checks before sleeping; busy mode ignores the spin count. The default remains `busy` for compatibility with the initial Phase 1 behavior. Strategies are chosen independently per process before it claims its role.

### Recovery demo

The `sub` command's last optional argument delays its explicit ACK, allowing a crash right in the vulnerable window:

```bash
./build/ironbus create --name recovery --capacity 8
./build/ironbus pub --name recovery --count 8
./build/ironbus sub --name recovery --count 8 --timeout-ms 30000 --ack-delay-ms 350000 &
SUB_PID=$!
sleep 0.45
kill -9 "$SUB_PID" || true
./build/ironbus stats --name recovery
# Restart with the outstanding count shown by stats (8 if no message was ACKed):
./build/ironbus sub --name recovery --count 8
./build/ironbus stats --name recovery
./build/ironbus unlink --name recovery
```

Messages already ACKed do not reappear. If a consumer is killed after receiving but before ACKing, the message is available again to the new consumer. The integration suite tests this deterministically enough for a development machine.

## Guarantees and limits

| Property | v0.1 behavior |
|---|---|
| Ordering | Strict monotonically increasing per-ring sequence |
| Capacity | Bounded; publisher backpressures when full |
| Publisher | Single-owner OFD lock, automatically released on crash |
| Subscribers | Fixed roster, each identity has a single-owner OFD lock released on crash |
| Consumer crash | Its unacknowledged messages redelivered after same-ID restart; its cursor still gates reuse |
| Publisher crash | Previously published data remains in shared memory; restart continues from cursor |
| Delivery | **At least once until acknowledged**, provided shared-memory object remains available |
| End-to-end exactly once | **Not provided**; app must use idempotence/durable effects if needed |
| Data durability | In RAM (POSIX shared-memory object); not power-loss durable |
| Fanout | Broadcast to every registered subscriber; slowest ACK gates reuse |
| Zero-copy receiving | Not yet implemented; safe copy prevents slot reuse races |
| Cross-host delivery | Not supported |
| Portability | Linux x86-64 only; not native macOS/Windows |

**Important:** ISO C++ alone does not specify inter-process behavior for `std::atomic`. This implementation relies on lock-free atomics in shared mappings on the supported Linux x86-64 platform, with acquire/release ordering. It refuses incompatible layouts. Do not call it universally portable C++.

**Security/operational limits:** The named object uses mode `0600` and contains readable application data. It's a single-user local IPC primitive, not a hardened multi-tenant service. Object destruction is a maintenance action and should not race a new attachment. A creator crash during initialization leaves a segment rejected as unsafe; explicitly unlink it to retry. The prototype does not include a system-wide ownership supervisor or durable log.

## Roadmap

- **Phase 0: Foundation (complete)** — C++20/CMake layout, strict warnings, Linux CI, sanitizer option, developer container, and engineering docs. Linux x86-64 build and tests passed.
- **Phase 1: Correct SPSC shared-memory IPC (complete)** — POSIX shared memory, fixed slots, acquire/release cursors, bounded backpressure, role locks, crash/redelivery behavior, CLI, subprocess tests, and an initial benchmark. All documented acceptance tests passed on Linux x86-64.
- **Phase 2: Wakeup strategies and polling (complete)** — per-endpoint busy, Linux process-shared futex, and bounded adaptive spin-then-futex policies; CI acceptance passed.
- **Phase 3: Multi-consumer architecture (complete)** — bounded broadcast SPMC, stable per-subscriber cursors, admission and explicit quiescent removal. Native Linux x86-64 Release and ASan/UBSan CI passed. See [PHASE3_DESIGN.md](docs/PHASE3_DESIGN.md).
- **Phase 4: Failure recovery and lifecycle hardening (complete)** — 16/16 acceptance checks passed on Purdue's `eceprog3` Linux x86-64 server: all ten fault-injection transitions and six additional lifecycle/layout/inspection checks. Recovery preserved unacknowledged messages for redelivery. The acceptance scripts run in bounded Linux CI; see [PHASE4_DESIGN.md](docs/PHASE4_DESIGN.md).
- **Phase 5: Performance engineering (pending)** — controlled native Linux baselines are still required before profiling cache-line contention, affinity, NUMA, and tail latency or making performance changes.
- **Phase 6: Market-data integration** — typed market-data examples and integrations after transport and schema contracts stabilize.

See [ARCHITECTURE.md](docs/ARCHITECTURE.md), [PLAN.md](docs/PLAN.md), and [PHASE4_DESIGN.md](docs/PHASE4_DESIGN.md) for invariants and acceptance gates. On macOS, run `./scripts/linux-build.sh`; it builds in a Linux x86-64 container into `build-linux-x86_64`. Apple Silicon runs this through emulation, so use native Linux hardware for meaningful performance measurements. The native macOS build intentionally fails because Linux OFD locks are required.
