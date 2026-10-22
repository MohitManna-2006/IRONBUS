# Phase 3 fanout benchmark notes

## Measurement contract

`ironbus_bench` launches one publisher and the requested number of independent subscriber processes. It synchronizes startup on subscriber registration. The publisher publishes a message and waits for **all** subscribers to acknowledge it before publishing the next message. Reported throughput is therefore serial broadcast-retirement cadence, not batched peak ring throughput. Latency is measured by the publisher from immediately before publish to observation that all subscriber ACKs have advanced, using `std::chrono::steady_clock`. It includes IPC scheduling, each endpoint's wait policy, and fanout completion. It is not an isolated store/load latency.

CPU seconds combine the parent and child `getrusage` user/system counters where available. The run reports payload size, ring capacity, subscriber count, wait strategies, spin budget, affinity, message count, architecture, kernel, and elapsed duration. Scheduling priority and CPU topology remain environmental inputs. Container and emulated measurements are functional experiments, not estimates of bare-metal deployment behavior.

## Recorded x86-64 Docker emulation sample

Host: Apple Silicon macOS, Linux x86-64 Docker emulation, 2 virtual CPUs. Container: `ironbus-linux-dev`. Release build. Workload: 10,000 messages, 64-byte payload, capacity 1,024, adaptive publisher and adaptive subscribers, 1,000 spin checks, no explicit affinity.

| Subscribers | Elapsed seconds | Retired broadcasts/s | p50 all-ACK ns | p99 all-ACK ns | p99.9 all-ACK ns | Combined CPU seconds |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 0.0528866 | 189,084 | 1,875 | 54,334 | 473,084 | 0.156884 |
| 4 | 12.8602 | 777.595 | 1,068,666 | 3,741,250 | 8,842,458 | 24.1086 |
| 16 | not completed | — | — | — | — | — |

The 16-reader attempt was terminated because it continued impractically long under emulation; it is not a result. The strong change between one and four readers is an observation from this setup, not evidence of a general scaling law. It likely reflects scheduler and emulation effects with more runnable processes than available virtual CPUs. The sample is unsuitable for tuning or trading-system capacity claims. Re-run the reader-count sweep on an otherwise idle native Linux x86-64 host, record CPU model/kernel/cpuset and repetitions, and retain raw run output before interpreting scaling.

The key architectural cost is clear independently of this sample: one shared ring deliberately applies slowest-reader backpressure, and publication wakes all futex sleepers. Phase 5 should compare this design against explicit alternative contracts such as per-reader rings or a wake relay, while measuring memory/copy costs and tail latency. No such alternative is implemented in Phase 3.
