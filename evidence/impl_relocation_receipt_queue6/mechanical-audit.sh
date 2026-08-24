#!/usr/bin/env bash
set -euo pipefail

repo=/root/cj_build/cangjie_runtime_wt/w_impl_relocation_receipt_queue6
baseline=d885495ebec6b720c7399fb3938b69107fe77456
old_lane=refs/lanes/impl_relocation_receipt_queue5
cd "$repo"

echo '=== DIRECT QUEUE LIFETIME AND RECEIPT CONSUMERS ==='
git grep -n -E 'BeginWorkers|SynchronizePoll|relocationRequestQueue|GetRelocationRequestQueue' -- \
    runtime/src/Heap/Allocator runtime/src/Heap/Collector runtime/src/Heap/WCollector

echo '=== UPWARD RUNTIME ENTRY CHAIN ==='
git grep -n -E 'ForwardFromRegions|ForwardFromSpace|EvacuateYoungRegions|PrepareForwardTable|PrepareFromRegionList' -- \
    runtime/src/Heap/Allocator runtime/src/Heap/Collector runtime/src/Heap/WCollector

echo '=== TESTS ADDED RELATIVE TO CURRENT MAIN ==='
git grep -h 'GC_TEST(' "$baseline" -- runtime/tests/gc_unit | \
    sed -E 's/.*GC_TEST\(([A-Za-z0-9_]+),[[:space:]]*([A-Za-z0-9_]+)\).*/\1.\2/' | sort -u > /tmp/queue6-main-tests.txt
git grep -h 'GC_TEST(' HEAD -- runtime/tests/gc_unit | \
    sed -E 's/.*GC_TEST\(([A-Za-z0-9_]+),[[:space:]]*([A-Za-z0-9_]+)\).*/\1.\2/' | sort -u > /tmp/queue6-head-tests.txt
comm -13 /tmp/queue6-main-tests.txt /tmp/queue6-head-tests.txt

echo '=== TESTS DELETED RELATIVE TO CURRENT MAIN ==='
comm -23 /tmp/queue6-main-tests.txt /tmp/queue6-head-tests.txt

echo '=== QUEUE5 TEST REPLACEMENTS (INFORMATIONAL) ==='
git grep -h 'GC_TEST(' "$old_lane" -- runtime/tests/gc_unit | \
    sed -E 's/.*GC_TEST\(([A-Za-z0-9_]+),[[:space:]]*([A-Za-z0-9_]+)\).*/\1.\2/' | sort -u > /tmp/queue6-old-tests.txt
echo ADDED_VS_QUEUE5
comm -13 /tmp/queue6-old-tests.txt /tmp/queue6-head-tests.txt
echo DELETED_VS_QUEUE5
comm -23 /tmp/queue6-old-tests.txt /tmp/queue6-head-tests.txt

echo '=== CURRENT MAIN MARKERS PRESERVED ==='
git grep -n -E 'GC_TEST\(MemMapContract, (FallbackRegistryPreservesEveryAcquiredReservation|RegionManagerInactiveAllocationUsesMemMapOwner)|GC_TEST\(RangeRegistry|ArmedMissIsNullNotGeometry' HEAD -- runtime/tests/gc_unit
git grep -n -E 'RangeRegistry|RelocationRequestQueue|ReceiptAllowsForwarded' HEAD -- \
    runtime/tests/gc_unit/run_standalone.sh runtime/src/CMakeLists.txt runtime/config.cmake

echo '=== CURRENT MAIN PRODUCT/TEST PATHS MISSING FROM DELIVERY ==='
comm -23 \
    <(git ls-tree -r --name-only "$baseline" runtime/src runtime/tests | sort -u) \
    <(git ls-tree -r --name-only HEAD runtime/src runtime/tests | sort -u)

echo '=== CONFLICT FILE EXACT DIFF AGAINST CURRENT MAIN ==='
git diff --unified=0 "$baseline"..HEAD -- runtime/src/CMakeLists.txt runtime/tests/gc_unit/run_standalone.sh
