#!/usr/bin/env bash
# IRONBUS Phase 4: remaining lifecycle / inspection acceptance checks.
# Run on an authorized native Linux x86-64 host from the IRONBUS repo root.
# This is a low-volume correctness test, not a performance benchmark.
set -Eeuo pipefail

[[ "$(uname -s)" == Linux && "$(uname -m)" == x86_64 ]] || { echo 'Need Linux x86_64' >&2; exit 2; }
for tool in timeout sha256sum truncate dd ps; do
  command -v "$tool" >/dev/null || { echo "Missing $tool" >&2; exit 2; }
done
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${1:-${IRONBUS_BINARY:-$ROOT/build/ironbus}}"
[[ -x "$BIN" ]] || { echo 'Missing build/ironbus; build Release first' >&2; exit 2; }
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
OUTPUT="$ROOT/build/phase4-remaining-${STAMP}-$$"
mkdir -p "$OUTPUT"
SUMMARY="$OUTPUT/summary.tsv"
printf 'case\tresult\tdetail\n' > "$SUMMARY"
{
  date -u -Is
  uname -a
  git -C "$ROOT" rev-parse HEAD 2>/dev/null || true
  git -C "$ROOT" status --short 2>/dev/null || true
  compiler="${IRONBUS_COMPILER:-${CXX:-c++}}"
  printf 'Compiler: %s\n' "$compiler"
  "$compiler" --version 2>&1 | sed -n '1p'
} > "$OUTPUT/environment.txt"
BASE="p4remain_$(id -un | tr -cd 'a-zA-Z0-9_')_$$_${STAMP}"
active_name=''
background_pid=''
LOG=''
LAST_RC=0
LAST_OUT=''
REASON=''
PASSED=0
FAILED=0

cleanup() {
  if [[ -n "$background_pid" ]]; then
    kill -CONT "$background_pid" 2>/dev/null || true
    kill -KILL "$background_pid" 2>/dev/null || true
    wait "$background_pid" >/dev/null 2>&1 || true
    background_pid=''
  fi
  if [[ -n "$active_name" ]]; then
    "$BIN" unlink --name "$active_name" >> "$LOG" 2>&1 || true
    active_name=''
  fi
}
trap cleanup EXIT INT TERM

run() {
  local rc=0
  printf '$ ' >> "$LOG"; printf '%q ' "$@" >> "$LOG"; printf '\n' >> "$LOG"
  LAST_OUT="$(timeout -k 1s 6s "$@" 2>&1)" || rc=$?
  LAST_RC=$rc
  printf '%s\n[exit=%s]\n\n' "$LAST_OUT" "$LAST_RC" >> "$LOG"
}
expect_rc() { [[ "$LAST_RC" -eq "$1" ]] || { REASON="wanted rc=$1, received $LAST_RC"; return 1; }; }
expect_text() { [[ "$LAST_OUT" == *"$1"* ]] || { REASON="missing output: $1"; return 1; }; }
reject_text() { [[ "$LAST_OUT" != *"$1"* ]] || { REASON="unexpected output: $1"; return 1; }; }

create_ready() {
  run "$BIN" create --name "$active_name" --capacity 8 --max-subscribers 2
  expect_rc 0
}
inspect_has() {
  run "$BIN" inspect --name "$active_name"
  expect_rc 0 && expect_text "$1"
}
setup_reader() {
  create_ready || return 1
  run env IRONBUS_FAULT_POINT=member_registered "$BIN" sub --name "$active_name" --id reader-a --count 1 --timeout-ms 500
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

# Starts a child which SIGSTOPs itself at a named private hook; captures PID.
start_paused() {
  local point="$1"
  shift
  env IRONBUS_FAULT_POINT="$point" IRONBUS_FAULT_ACTION=pause "$BIN" "$@" >> "$LOG" 2>&1 &
  background_pid=$!
  printf 'paused-child-pid=%s point=%s\n' "$background_pid" "$point" >> "$LOG"
}
await_stopped() {
  local i state
  for ((i=0;i<100;i++)); do
    state="$(ps -o stat= -p "$background_pid" 2>/dev/null || true)"
    if [[ "$state" == *T* ]]; then return 0; fi
    if ! kill -0 "$background_pid" 2>/dev/null; then
      REASON="child exited instead of SIGSTOP; state=$state"
      return 1
    fi
    sleep 0.02
  done
  REASON='child did not reach SIGSTOP within 2 seconds'
  return 1
}
finish_paused() {
  kill -KILL "$background_pid" 2>/dev/null || true
  wait "$background_pid" >> "$LOG" 2>&1 || true
  background_pid=''
}

case_paused_publisher_lock() {
  setup_reader || return 1
  start_paused publish_before_commit pub --name "$active_name" --count 1 --timeout-ms 1000
  await_stopped || return 1
  inspect_has 'published=0' || return 1
  expect_text 'publisher_active=true' || return 1
  run "$BIN" pub --name "$active_name" --count 1 --timeout-ms 250
  expect_rc 1 || return 1
  expect_text 'publisher role is already held' || return 1
  run "$BIN" forget --name "$active_name" --id reader-a
  expect_rc 1 || return 1
  expect_text 'stop the publisher' || return 1
  finish_paused
  inspect_has 'publisher_active=false' || return 1
  publish_one || return 1
  consume_one || return 1
  inspect_has 'subscriber id=reader-a acknowledged=1 active=false'
}
case_paused_subscriber_lock() {
  setup_reader || return 1
  publish_one || return 1
  start_paused ack_before_commit sub --name "$active_name" --id reader-a --count 1 --timeout-ms 1000
  await_stopped || return 1
  inspect_has 'subscriber id=reader-a acknowledged=0 active=true' || return 1
  run "$BIN" sub --name "$active_name" --id reader-a --count 1 --timeout-ms 250
  expect_rc 1 || return 1
  expect_text 'subscriber identity is already active' || return 1
  run "$BIN" forget --name "$active_name" --id reader-a
  expect_rc 1 || return 1
  expect_text 'stop the subscriber' || return 1
  finish_paused
  inspect_has 'subscriber id=reader-a acknowledged=0 active=false' || return 1
  consume_one || return 1
  inspect_has 'subscriber id=reader-a acknowledged=1 active=false'
}
case_quiescent_forget() {
  setup_reader || return 1
  publish_one || return 1
  inspect_has 'subscriber id=reader-a acknowledged=0 active=false' || return 1
  run "$BIN" forget --name "$active_name" --id reader-a
  expect_rc 0 || return 1
  run "$BIN" stats --name "$active_name"
  expect_rc 0 || return 1
  expect_text 'registered_subscribers=0' || return 1
  run "$BIN" inspect --name "$active_name"
  expect_rc 0 || return 1
  reject_text 'subscriber id=reader-a' || return 1
  run "$BIN" pub --name "$active_name" --count 1 --timeout-ms 250
  expect_rc 1 || return 1
  expect_text 'publishing requires at least one registered subscriber'
}
case_truncated_layout() {
  create_ready || return 1
  local shm="/dev/shm/ironbus_${active_name}"
  [[ -f "$shm" ]] || { REASON="shared memory not exposed at $shm"; return 1; }
  truncate -s 16 "$shm"
  run "$BIN" inspect --name "$active_name"
  expect_rc 1 || return 1
  expect_text 'truncated or still initializing'
}
case_corrupt_layout() {
  create_ready || return 1
  local shm="/dev/shm/ironbus_${active_name}" before after
  [[ -f "$shm" ]] || { REASON="shared memory not exposed at $shm"; return 1; }
  # Header version starts at offset 8 after uint64_t magic on Linux x86-64.
  printf '\000\000\000\000' | dd of="$shm" bs=1 seek=8 conv=notrunc status=none
  before="$(sha256sum "$shm")"
  run "$BIN" inspect --name "$active_name"
  expect_rc 1 || return 1
  expect_text 'incompatible or corrupt' || return 1
  after="$(sha256sum "$shm")"
  [[ "$before" == "$after" ]] || { REASON='inspect mutated corrupted memory'; return 1; }
}
case_inspect_is_read_only() {
  setup_reader || return 1
  publish_one || return 1
  local shm="/dev/shm/ironbus_${active_name}" before after
  [[ -f "$shm" ]] || { REASON="shared memory not exposed at $shm"; return 1; }
  before="$(sha256sum "$shm")"
  inspect_has 'subscriber id=reader-a acknowledged=0 active=false' || return 1
  run "$BIN" stats --name "$active_name"
  expect_rc 0 || return 1
  after="$(sha256sum "$shm")"
  [[ "$before" == "$after" ]] || { REASON='inspect/stats mutated memory'; return 1; }
}

test_case() {
  local label="$1" fn="$2"
  active_name="${BASE}_${label}"
  LOG="$OUTPUT/${label}.log"
  REASON=''
  printf 'CASE %s\n' "$label" > "$LOG"
  if "$fn"; then
    printf 'PASS\t%s\n' "$label"
    printf '%s\tPASS\tverified\n' "$label" >> "$SUMMARY"
    PASSED=$((PASSED+1))
  else
    printf 'FAIL\t%s: %s\n' "$label" "${REASON:-see log}"
    printf '%s\tFAIL\t%s\n' "$label" "${REASON:-see log}" >> "$SUMMARY"
    FAILED=$((FAILED+1))
  fi
  cleanup
}

echo 'IRONBUS Phase 4 remaining acceptance tests'
echo "Evidence: $OUTPUT"
test_case paused_publisher_lock case_paused_publisher_lock
test_case paused_subscriber_lock case_paused_subscriber_lock
test_case quiescent_forget case_quiescent_forget
test_case truncated_layout case_truncated_layout
test_case corrupt_layout case_corrupt_layout
test_case inspect_read_only case_inspect_is_read_only
printf '\nResults: %d passed, %d failed\n' "$PASSED" "$FAILED"
ARCHIVE="${OUTPUT}.tar.gz"
tar -czf "$ARCHIVE" -C "$(dirname "$OUTPUT")" "$(basename "$OUTPUT")"
echo "Shareable archive: $ARCHIVE"
[[ "$FAILED" -eq 0 ]]
