# Architecture and invariants

## Scope

Phase 1 is one publisher and one subscriber sharing one bounded ring. Linux POSIX shared memory provides the mapping. Linux open-file-description (OFD) byte-range locks on the shared-memory file descriptor provide exclusive process roles and are released by the kernel when the owning open file description closes or its process exits.

The shared object stores offsets and fixed-layout values only. It contains no process-local pointers. All participants must use a compatible Linux ABI, endianness, compiler atomic representation, and structure layout. The implementation validates its magic, version, header size, slot size, capacity, and lock-free atomic support before attachment.

## Sequence protocol

`published` is the highest completely published sequence; `acknowledged` is the highest sequence whose consumer has completed and explicitly acknowledged it. Both begin at zero. For the next sequence `n = published + 1`:

1. The publisher may write only if `n - acknowledged <= capacity`.
2. It writes the fixed slot at `(n - 1) & (capacity - 1)`.
3. It release-stores `published = n` only after the slot is complete.
4. The subscriber acquire-loads `published`, copies the next unacknowledged slot, and checks the embedded sequence.
5. The subscriber release-stores `acknowledged = n` only after processing succeeds.

The publisher acquire-loads `acknowledged` before reusing a slot. This bounds memory and prevents overwriting an unacknowledged message. Polling waits have a finite deadline and return a timeout error; they do not silently drop messages.

## Delivery and recovery

Delivery is at least once until explicit acknowledgement while the shared-memory object survives. A subscriber crash after copy/processing but before acknowledgement can cause redelivery. Applications requiring exactly-once effects must make those effects idempotent or coordinate their own durable transaction with acknowledgement.

A subscriber restart resumes from `acknowledged + 1`. A publisher restart reacquires the exclusive publisher role and continues at `published + 1`; this assumes the segment was retained and validated. Destroy/unlink is an operator action and must not race live participants: unlink prevents future name-based attachment but does not invalidate existing mappings.

A process killed while initializing can leave a segment in `INITIALIZING`; attachers reject it. The operator must explicitly unlink that segment before retrying creation. There is no automatic takeover of a live but stalled role: OFD locks remain held until the owning open file description closes. Do not fork a process after it claims a role unless the child immediately closes the inherited bus descriptor or execs; an inherited descriptor shares the OFD lock until every copy closes. This avoids unsafe liveness guesses.

## Memory ordering and assumptions

The design uses lock-free `std::atomic<uint64_t>` and `std::atomic<uint32_t>` in a shared `mmap`, with acquire/release ordering. ISO C++ does not guarantee inter-process synchronization semantics for `std::atomic`; this implementation relies on Linux implementations where lock-free atomics operate on coherent shared mappings. Creation and attachment reject platforms where the required atomic types are not lock-free. The intended deployment target is Linux x86-64; Linux CI may also exercise compatible architectures, but that does not expand the supported ABI claim.

The message slot is fixed at 256 bytes: a 64-bit sequence, 32-bit length, 32-bit reserved field, and 240-byte payload. The public maximum payload is 240 bytes. Producers copy into shared memory and consumers copy out; no zero-copy view is exposed, preventing the slot from being reused while application code retains a view.

## Cache layout

Published and acknowledged counters occupy separate 64-byte aligned cache lines to reduce direct false sharing. The slot array follows the validated header. This is a simple initial layout, not a claim that 64 bytes is universal across CPUs.

## Phase 2 waiting

Each endpoint selects a process-local `busy_poll`, `futex`, or `adaptive` wait policy before claiming its role. Published and acknowledged sequences remain the predicates. Separate 32-bit data and space generation words are only sleep-transition hints, placed on their own cache lines and checked for lock-free operation at attachment. A publisher release-publishes the slot, increments the data generation, and wakes the subscriber; an ACK release-stores the cursor, increments the space generation, and wakes the publisher. Waiters check the predicate, sample the generation, check the predicate again, then wait on the sampled value. This closes the check-to-sleep race because a changed word makes `FUTEX_WAIT` return without sleeping. Spurious wakeups and interrupts return to the predicate check.

Futex syscalls use process-shared operations. Adaptive mode spins for its finite configured check budget, then follows the same futex path. All public waits use a `steady_clock` deadline. Generation wrap does not affect message correctness; the wider sequence cursors remain authoritative. A segment with the previous layout version is rejected rather than interpreted as the new format.

Wakeups are not a crash detector. If a process stops while holding its role, the other endpoint observes only its finite wait timeout; the lock is released by process exit/descriptor closure. If a process dies after committing a cursor but before waking its peer, the cursor is still authoritative and a restart can recover from it. Automatic takeover, health heartbeats, and wait-free wake guarantees are outside this milestone.
