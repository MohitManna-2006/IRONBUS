# IRONBUS Engineering Plan

## Phase 0: Foundation — complete (2026-10-22)

- Establish a C++20 CMake project with strict warnings, opt-in development sanitizers, release optimization, Linux CI, and a reproducible Linux container workflow.
- Keep the public API in `include/ironbus`, implementation in `src`, command-line tools in `apps`, correctness/integration checks in `tests`, and measurements in `bench`.
- Acceptance: Linux x86-64 release configure/build and automated tests passed in a Docker x86-64 container on 2026-10-22. The macOS host executes that container through emulation.

## Phase 1: Correct SPSC shared-memory IPC — complete (2026-10-22)

- One publisher and one subscriber communicate through fixed-size slots in a POSIX shared-memory object.
- Use monotonic published and acknowledged sequences, acquire/release atomics, bounded backpressure, role exclusion, validation, and restart from the last acknowledged sequence.
- Reject unknown, incomplete, or incompatible layouts. Explicitly state Linux ABI and lock-free atomic assumptions.
- Acceptance: ordering/integrity, wraparound/full/empty/backpressure, independent processes, crash/redelivery, publisher restart, truncated-layout rejection, and both role-contention checks passed in the bounded Linux x86-64 integration suite on 2026-10-22. CLI operations work between separately launched processes.

## Phase 2: Wakeup strategies and polling — complete (2026-10-22)

- Add process-shared Linux futex wait/wake with separate data and space generations while keeping the 64-bit sequence cursors authoritative.
- Expose busy polling, futex waiting, and bounded-spin adaptive waiting per endpoint. Preserve finite timeout behavior and reject unsafe layouts.
- Add optional benchmark CPU affinity and comparisons that record throughput, publish-to-ACK p50/p99/p99.9, CPU time where available, and environment.
- Acceptance: mixed policy matrix, both startup orders, empty/full/ACK timeouts, role exclusivity, crash redelivery, release benchmark smoke, and bounded benchmark argument validation pass. Linux x86-64 GitHub Actions release and sanitizer jobs passed on 2026-10-22. Local x86-64 Docker-emulated release tests also pass; local sanitizer execution is limited by emulation process kills.

See [PHASE2_DESIGN.md](PHASE2_DESIGN.md) for the wait protocol and failure analysis. Mark Phase 2 complete only after the acceptance checks pass.
See [PHASE2_BENCHMARK_NOTES.md](PHASE2_BENCHMARK_NOTES.md) for one explicitly non-representative emulated comparison sample.

## Phase 3: Multi-consumer architecture — complete (2026-10-22)

Implement the broadcast SPMC contract in [PHASE3_DESIGN.md](PHASE3_DESIGN.md): stable per-subscriber cursors, explicit admission, slowest-reader backpressure, safe quiescent late joins, and explicit removal with no automatic stale-member expiry. Acceptance passed: cross-process fanout, slow-reader gating, restart/redelivery, late admission, explicit forgetting, bounded role conflicts, benchmark fanout smoke, plus native Linux x86-64 Release and ASan/UBSan CI on 2026-10-22. Local x86-64 Docker-emulated Release tests also passed. Performance samples remain explicitly non-representative of bare metal.

## Phase 4: Failure recovery and lifecycle hardening

The recovery contract and fault model are documented in [PHASE4_DESIGN.md](PHASE4_DESIGN.md). Deterministic development-only fault injection and read-only roster inspection are implemented. The remaining gate is to exercise every persistent transition on native Linux x86-64, address any failures, and record the evidence before marking this phase complete.

## Phase 5: Performance engineering

The repeatable fanout/affinity/NUMA sweep runner and methodology are documented in [PHASE5_PERFORMANCE.md](PHASE5_PERFORMANCE.md). Native Linux baselines and measured, evidence-backed optimization remain the acceptance gate; container-emulated runs are not performance evidence.

## Phase 6: Market-data integration

Add typed market-data examples and integrations after the transport contract and schema/version policy are stable.
