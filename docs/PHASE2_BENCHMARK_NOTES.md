# Phase 2 benchmark sample

One functional comparison was run on 2026-10-22 using the Linux x86-64 container on an Apple Silicon macOS host. Docker used x86-64 emulation. The run used 10,000 messages, 64-byte payloads, capacity 1,024, no CPU affinity, and spin budget 1,000. Each endpoint used the named policy. The benchmark timestamped immediately before publish and measured until the publisher observed the corresponding ACK through its selected wait policy.

| Policy | Messages/s | ACK p50 | ACK p99 | ACK p99.9 | Combined process CPU time |
| --- | ---: | ---: | ---: | ---: | ---: |
| Busy | 387,011 | 1,833 ns | 5,709 ns | 77,209 ns | 0.112 s |
| Futex | 17,605 | 50,542 ns | 198,625 ns | 594,083 ns | 0.426 s |
| Adaptive | 423,318 | 1,792 ns | 3,959 ns | 31,667 ns | 0.088 s |

These are the actual outputs from one run, not controlled bare-metal results. Scheduler placement and host emulation materially affect them, so they must not be used to rank policies for deployment. The futex result demonstrates the latency cost of sleeping for this particular short request/ACK cadence; a workload with longer idle periods may have different CPU behavior. Re-run multiple times on otherwise idle bare-metal Linux and preserve the full environment before drawing conclusions.
