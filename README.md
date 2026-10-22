# IRONBUS

**No sockets on the hot path. No silent overwrites. ACK before reuse.**

A Linux x86-64, C++20, cross-process shared-memory messaging system.

> **Status: Phase 0 and Phase 1 complete.** Linux x86-64 release build and acceptance tests pass. Multi-subscriber fanout, futex wakeups, broker discovery, and zero-copy payload views are not implemented.

## Why this exists

A market-data process needs to distribute normalized events to a separate trading process with minimal transport overhead. A slow or dead consumer must not cause silent message corruption. IRONBUS is a focused systems-engineering laboratory for process-shared atomics, cache coherence, memory ordering, backpressure, failure recovery, and measurable latency.

## Architecture today

```text
   publisher process                    subscriber process
   -----------------                    ------------------
   claim OFD producer lock              claim OFD subscriber lock
          |                                       |
          v                                       v
  write Slot[N] payload             acquire-load published cursor
  release-store published            copy and validate Slot[N]
          |                          process message / test hook
          +---- POSIX shared memory ----> release-store ACK cursor
                  bounded SPSC ring

             publisher cannot reuse Slot[N]
             until subscriber ACKs it
```

**Design choices:** fixed-size 256-byte slots, up to 240 payload bytes, power-of-two ring capacity, no dynamic allocation in the library's publish/receive operations, independent 64-byte-aligned atomic cursors, exclusive publisher/subscriber roles enforced with Linux OFD locks, and exact ABI validation upon attach.

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

./build/ironbus create --name demo --capacity 1024
# Terminal 1
./build/ironbus sub --name demo --count 100000
# Terminal 2
./build/ironbus pub --name demo --count 100000
# Later
./build/ironbus stats --name demo
./build/ironbus unlink --name demo
```

The executable communicates **between actual processes** (not threads in a single executable). Run the subscriber first or run the publisher first. Without a subscriber, the publisher fills the bounded ring and waits up to 30 seconds by default; use `--timeout-ms` to change the deadline. A stalled live process retains its exclusive role lock until it exits or closes its handle. No messages are silently dropped.

### Benchmark

```bash
./build/ironbus_bench 1000000 4096 64
```

The harness launches independent publisher and subscriber processes. It reports messages per second and **publisher-observed publish-to-ACK completion latency** (p50, p99, p99.9) using `steady_clock`; CPU usage is not collected by the harness. Timing includes scheduler and polling effects. Run under `/usr/bin/time -v` for process CPU accounting. These are not single-operation ring overhead or NIC wire-to-wire latencies. Record architecture, kernel, load, affinity, and multiple runs before drawing conclusions. Docker measurements are not representative of bare-metal latency.

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
| Subscriber | Single-owner OFD lock, automatically released on crash |
| Consumer crash | Unacknowledged messages redelivered after restart |
| Publisher crash | Previously published data remains in shared memory; restart continues from cursor |
| Delivery | **At least once until acknowledged**, provided shared-memory object remains available |
| End-to-end exactly once | **Not provided**; app must use idempotence/durable effects if needed |
| Data durability | In RAM (POSIX shared-memory object); not power-loss durable |
| Multi-consumer fanout | Not yet implemented |
| Zero-copy receiving | Not yet implemented; safe copy prevents slot reuse races |
| Cross-host delivery | Not supported |
| Portability | Linux x86-64 only; not native macOS/Windows |

**Important:** ISO C++ alone does not specify inter-process behavior for `std::atomic`. This implementation relies on lock-free atomics in shared mappings on the supported Linux x86-64 platform, with acquire/release ordering. It refuses incompatible layouts. Do not call it universally portable C++.

**Security/operational limits:** The named object uses mode `0600` and contains readable application data. It's a single-user local IPC primitive, not a hardened multi-tenant service. Object destruction is a maintenance action and should not race a new attachment. A creator crash during initialization leaves a segment rejected as unsafe; explicitly unlink it to retry. The prototype does not include a system-wide ownership supervisor or durable log.

## Roadmap

- **Phase 0: Foundation (complete)** — C++20/CMake layout, strict warnings, Linux CI, sanitizer option, developer container, and engineering docs. Linux x86-64 build and tests passed.
- **Phase 1: Correct SPSC shared-memory IPC (complete)** — POSIX shared memory, fixed slots, acquire/release cursors, bounded backpressure, role locks, crash/redelivery behavior, CLI, subprocess tests, and an initial benchmark. All documented acceptance tests passed on Linux x86-64.
- **Phase 2: Wakeup strategies and polling** — compare bounded busy polling with Linux futex wakeups; record CPU and latency effects.
- **Phase 3: Multi-consumer architecture** — specify per-subscriber cursors, admission, backpressure, and abandoned-subscriber reclamation before implementation.
- **Phase 4: Failure recovery and lifecycle hardening** — fault injection, initialization/lifecycle hardening, operational inspection, and explicit recovery policy.
- **Phase 5: Performance engineering** — cache-line contention, affinity, NUMA, and tail-latency profiling with reproducible bare-metal methodology.
- **Phase 6: Market-data integration** — typed market-data examples and integrations after transport and schema contracts stabilize.

See [ARCHITECTURE.md](docs/ARCHITECTURE.md) and [PLAN.md](docs/PLAN.md) for invariants and acceptance gates. On macOS, run `./scripts/linux-build.sh`; it builds in a Linux x86-64 container into `build-linux-x86_64`. Apple Silicon runs this through emulation, so use native Linux hardware for meaningful performance measurements. The native macOS build intentionally fails because Linux OFD locks are required.
