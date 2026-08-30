#!/usr/bin/env bash
set -euo pipefail

SO="${1:?usage: check_driver_receipt_wiring.sh /path/to/libcangjie-runtime.so}"
test -f "$SO" || { echo "DRIVER_RECEIPT_WIRING_FAIL missing_so=$SO" >&2; exit 2; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
nm --defined-only "$SO" | c++filt > "$tmp/nm.txt"
objdump -d -C "$SO" > "$tmp/disassembly.txt"

/usr/bin/grep -F 'MapleRuntime::CopyCollector::RunGarbageCollection(unsigned long, MapleRuntime::GCReason)' \
    "$tmp/nm.txt" > /dev/null || {
    echo "DRIVER_RECEIPT_WIRING_FAIL missing_copy_collector" >&2
    exit 3
}
/usr/bin/grep -F 'MapleRuntime::CollectorResources::NotifyGCPhaseFinished(unsigned long)' \
    "$tmp/nm.txt" > /dev/null || {
    echo "DRIVER_RECEIPT_WIRING_FAIL missing_phase_completion" >&2
    exit 4
}

awk '
  /^[[:xdigit:]]+ <MapleRuntime::CopyCollector::RunGarbageCollection\(unsigned long, MapleRuntime::GCReason\)>:$/ {
    in_target = 1
  }
  in_target && /^[[:xdigit:]]+ <.*>:$/ &&
      $0 !~ /<MapleRuntime::CopyCollector::RunGarbageCollection\(unsigned long, MapleRuntime::GCReason\)>:$/ {
    exit
  }
  in_target { print }
' "$tmp/disassembly.txt" > "$tmp/copy_collector.txt"

anchor_count=$(/usr/bin/grep -c 'GcLog::Cycle(unsigned long' "$tmp/copy_collector.txt" || true)
phase_count=$(/usr/bin/grep -c 'CollectorResources::NotifyGCPhaseFinished' "$tmp/copy_collector.txt" || true)
legacy_count=$(/usr/bin/grep -c 'CollectorResources::NotifyGCFinished' "$tmp/copy_collector.txt" || true)
so_sha=$(sha256sum "$SO" | awk '{print $1}')
echo "DETAIL driver_receipt_wiring so=$SO so_sha=$so_sha anchor_count=$anchor_count phase_count=$phase_count legacy_count=$legacy_count"

if [[ "$anchor_count" -lt 1 ]]; then
    echo "DRIVER_RECEIPT_WIRING_FAIL disassembly_ruler_cannot_see_positive_anchor" >&2
    exit 5
fi
if [[ "$phase_count" -ne 1 || "$legacy_count" -ne 0 ]]; then
    echo "DRIVER_RECEIPT_WIRING_FAIL phase_count=$phase_count legacy_count=$legacy_count" >&2
    exit 6
fi
echo "DRIVER_RECEIPT_WIRING_OK"
