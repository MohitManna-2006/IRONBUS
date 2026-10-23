#!/usr/bin/env bash
# IRONBUS Phase 4: exercise all ten development fault points.
# Run on native Linux x86-64 from the IRONBUS repository root.
# Uses only short, isolated one-message workloads; no performance benchmarking.
set -Eeuo pipefail

if [[ "$(uname -s)" != Linux || "$(uname -m)" != x86_64 ]]; then
  echo "ERROR: requires native Linux x86-64" >&2
  exit 2
fi
command -v timeout >/dev/null || { echo "ERROR: GNU timeout is required" >&2; exit 2; }
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${1:-${IRONBUS_BINARY:-$ROOT/build/ironbus}}"
[[ -x "$BIN" ]] || { echo "ERROR: build/ironbus is missing. Build Release first." >&2; exit 2; }

STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
OUTPUT="$ROOT/build/phase4-acceptance-${STAMP}-$$"
mkdir -p "$OUTPUT"
SUMMARY="$OUTPUT/summary.tsv"
printf 'point\tresult\tdetail\n' > "$SUMMARY"
{
  printf 'UTC: %s\n' "$(date -u -Is)"
  printf 'Host: %s\n' "$(hostname)"
  uname -a
  git -C "$ROOT" rev-parse HEAD 2>/dev/null || true
  compiler="${IRONBUS_COMPILER:-${CXX:-c++}}"
  printf '\nCompiler: %s\n' "$compiler"
  "$compiler" --version 2>&1 | sed -n '1p'
} > "$OUTPUT/environment.txt"

BASE="p4_$(id -un | tr -cd 'a-zA-Z0-9_')_$$_${STAMP}"
active_name=''
LOG=''
LAST_RC=0
LAST_OUT=''
PASSED=0
FAILED=0
REASON=''

cleanup_object() {
  if [[ -n "$active_name" ]]; then
    # All command invocations are synchronous and timeout-bounded. Only the
    # current isolated test object is ever unlinked.
    "$BIN" unlink --name "$active_name" >> "$LOG" 2>&1 || true
    active_name=''
  fi
}
trap cleanup_object EXIT

run() {
  local rc=0
  {
    printf '$ '
    printf '%q ' "$@"
    printf '\n'
  } >> "$LOG"
  if LAST_OUT="$(timeout -k 1s 6s "$@" 2>&1)"; then
    rc=0
  else
    rc=$?
  fi
  LAST_RC=$rc
  printf '%s\n[exit=%d]\n\n' "$LAST_OUT" "$LAST_RC" >> "$LOG"
}

expect_rc() {
  if [[ $LAST_RC -ne $1 ]]; then
    REASON="Expected exit $1, got $LAST_RC"
    return 1
  fi
}
expect_text() {
  if [[ "$LAST_OUT" != *"$1"* ]]; then
    REASON="Output did not contain: $1"
    return 1
  fi
}
reject_text() {
  if [[ "$LAST_OUT" == *"$1"* ]]; then
    REASON="Output unexpectedly contained: $1"
    return 1
  fi
}

create_ready() {
  run "$BIN" create --name "$active_name" --capacity 8 --max-subscribers 2
  expect_rc 0
}
inspect_has() {
  run "$BIN" inspect --name "$active_name"
  expect_rc 0 && expect_text "$1"
}
# Establish an existing subscriber with a durable cursor, but no live process.
# Intentionally crash after registration. This is only test setup for later
# publication/ACK cases and does not count as a separate test.
setup_inactive_reader() {
  create_ready || return 1
  run env IRONBUS_FAULT_POINT=member_registered "$BIN" sub --name "$active_name" --id reader-a --count 1 --timeout-ms 300
  expect_rc 86 || return 1
  inspect_has 'subscriber id=reader-a acknowledged=0 active=false'
}
publish_one() {
  run "$BIN" pub --name "$active_name" --count 1 --timeout-ms 500
  expect_rc 0
}
consume_one() {
  run "$BIN" sub --name "$active_name" --id reader-a --count 1 --timeout-ms 500
  expect_rc 0
}

case_create_before_ready() {
  local point="$1"
  run env "IRONBUS_FAULT_POINT=$point" "$BIN" create --name "$active_name" --capacity 8
  expect_rc 86 || return 1
  run "$BIN" inspect --name "$active_name"
  expect_rc 1 || return 1
  expect_text 'shared memory'
}
case_create_after_ready() {
  run env IRONBUS_FAULT_POINT=create_after_ready "$BIN" create --name "$active_name" --capacity 8
  expect_rc 86 || return 1
  inspect_has 'published=0' || return 1
  expect_text 'publisher_active=false' || return 1
  expect_text 'capacity=8'
}
case_member_initializing() {
  create_ready || return 1
  run env IRONBUS_FAULT_POINT=member_initializing "$BIN" sub --name "$active_name" --id reader-a --count 1 --timeout-ms 500
  expect_rc 86 || return 1
  run "$BIN" inspect --name "$active_name"
  expect_rc 0 || return 1
  reject_text 'subscriber id=reader-a' || return 1
  # A fresh registration must reclaim the INITIALIZING slot.
  run "$BIN" sub --name "$active_name" --id reader-a --count 1 --timeout-ms 200
  expect_rc 1 || return 1 # correctly times out awaiting publication
  inspect_has 'subscriber id=reader-a acknowledged=0 active=false' || return 1
  publish_one || return 1
  consume_one || return 1
  inspect_has 'subscriber id=reader-a acknowledged=1 active=false'
}
case_member_registered() {
  setup_inactive_reader || return 1
  publish_one || return 1
  consume_one || return 1
  inspect_has 'subscriber id=reader-a acknowledged=1 active=false'
}
case_publish_before_commit() {
  setup_inactive_reader || return 1
  run env IRONBUS_FAULT_POINT=publish_before_commit "$BIN" pub --name "$active_name" --count 1 --timeout-ms 500
  expect_rc 86 || return 1
  inspect_has 'published=0' || return 1
  expect_text 'publisher_active=false' || return 1
  # Subscriber cannot see the uncommitted slot, even though bytes were written.
  run "$BIN" sub --name "$active_name" --id reader-a --count 1 --timeout-ms 200
  expect_rc 1 || return 1
  expect_text 'timed out' || return 1
  publish_one || return 1
  consume_one || return 1
  inspect_has 'subscriber id=reader-a acknowledged=1 active=false'
}
case_publish_after_commit() {
  setup_inactive_reader || return 1
  run env IRONBUS_FAULT_POINT=publish_after_commit "$BIN" pub --name "$active_name" --count 1 --timeout-ms 500
  expect_rc 86 || return 1
  inspect_has 'published=1' || return 1
  expect_text 'publisher_active=false' || return 1
  # Second publisher resumes at sequence 2 after crash.
  publish_one || return 1
  inspect_has 'published=2' || return 1
  run "$BIN" sub --name "$active_name" --id reader-a --count 2 --timeout-ms 500
  expect_rc 0 || return 1
  inspect_has 'subscriber id=reader-a acknowledged=2 active=false'
}
case_ack_before_commit() {
  setup_inactive_reader || return 1
  publish_one || return 1
  run env IRONBUS_FAULT_POINT=ack_before_commit "$BIN" sub --name "$active_name" --id reader-a --count 1 --timeout-ms 500
  expect_rc 86 || return 1
  inspect_has 'subscriber id=reader-a acknowledged=0 active=false' || return 1
  consume_one || return 1 # seq 1 must be redelivered
  inspect_has 'subscriber id=reader-a acknowledged=1 active=false'
}
case_ack_after_commit() {
  setup_inactive_reader || return 1
  publish_one || return 1
  run env IRONBUS_FAULT_POINT=ack_after_commit "$BIN" sub --name "$active_name" --id reader-a --count 1 --timeout-ms 500
  expect_rc 86 || return 1
  inspect_has 'subscriber id=reader-a acknowledged=1 active=false' || return 1
  # No replay of ACKed seq 1; next receive must time out before seq 2 exists.
  run "$BIN" sub --name "$active_name" --id reader-a --count 1 --timeout-ms 200
  expect_rc 1 || return 1
  expect_text 'timed out' || return 1
  publish_one || return 1
  consume_one || return 1
  inspect_has 'subscriber id=reader-a acknowledged=2 active=false'
}

test_one() {
  local point="$1" test_fn="$2"
  active_name="${BASE}_${point}"
  LOG="$OUTPUT/${point}.log"
  REASON=''
  printf 'TEST %s\n' "$point" > "$LOG"
  if "$test_fn" "$point"; then
    printf 'PASS\t%s\n' "$point"
    printf '%s\tPASS\tverified\n' "$point" >> "$SUMMARY"
    PASSED=$((PASSED+1))
  else
    printf 'FAIL\t%s (%s)\n' "$point" "$REASON"
    printf '%s\tFAIL\t%s\n' "$point" "${REASON:-check log}" >> "$SUMMARY"
    FAILED=$((FAILED+1))
  fi
  cleanup_object
}

printf 'IRONBUS Phase 4 native fault sweep\nLogs: %s\n\n' "$OUTPUT"
test_one create_after_open case_create_before_ready
test_one create_after_size case_create_before_ready
test_one create_before_ready case_create_before_ready
test_one create_after_ready case_create_after_ready
test_one member_initializing case_member_initializing
test_one member_registered case_member_registered
test_one publish_before_commit case_publish_before_commit
test_one publish_after_commit case_publish_after_commit
test_one ack_before_commit case_ack_before_commit
test_one ack_after_commit case_ack_after_commit

printf '\nResults: %d passed, %d failed (10 named fault points)\n' "$PASSED" "$FAILED"
printf 'Evidence: %s\n' "$OUTPUT"
ARCHIVE="${OUTPUT}.tar.gz"
tar -czf "$ARCHIVE" -C "$(dirname "$OUTPUT")" "$(basename "$OUTPUT")"
printf 'Shareable archive: %s\n' "$ARCHIVE"
printf 'Additional Phase 4 acceptance (pause-role exclusion, quiescent forget, malformed layouts) is not covered by this ten-point run.\n'
[[ $FAILED -eq 0 ]]
