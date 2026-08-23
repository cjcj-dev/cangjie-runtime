#!/usr/bin/env bash
# TRUST_STATE_KILL_PLAN Phase 1 static harness (positive + negative channels).
# Run from cangjie_runtime worktree root. Exit 0 = all checks pass.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
fail=0
pass() { echo "PASS  $*"; }
bad()  { echo "FAIL  $*"; fail=1; }

# ① TryUntag retired from Idle atomic read path
if grep -n 'TryUntagRefField' runtime/src/Heap/WCollector/IdleBarrier.cpp | grep -v '//' | grep -q .; then
  bad "① IdleBarrier still calls TryUntagRefField on product path"
else
  pass "① IdleBarrier product path has no TryUntagRefField call"
fi
# ① TryUntag write-back is GetAndTryTagRefField (current colour), not RefField<>(target)
if awk '/TryUntagRefField encounters invalid/,/^bool WCollector::|^}$/' \
      runtime/src/Heap/WCollector/WCollector.cpp | grep -q 'GetAndTryTagRefField(target)'; then
  pass "① TryUntag write-back uses GetAndTryTagRefField"
else
  bad "① TryUntag write-back is not coloured"
fi
if grep -n 'RefField<> newRef(target)' runtime/src/Heap/WCollector/WCollector.cpp | grep -q .; then
  bad "① residual plain RefField<>(target) in WCollector.cpp"
else
  pass "① no plain RefField<>(target) untag write-back"
fi

# ② named overloads present; void* address-guess gone
if grep -n 'RootSlotWriteback(BaseObject\* target, const void\*' runtime/src/Heap/WCollector/WCollector.h | grep -q .; then
  bad "② address-guess RootSlotWriteback(void*) still present"
else
  pass "② void* address-guess RootSlotWriteback removed"
fi
for t in 'const RefField<>&' 'const RootSlot&' 'const DerivedSlot&'; do
  if grep -n "RootSlotWriteback(BaseObject\* target, $t" runtime/src/Heap/WCollector/WCollector.h | grep -q .; then
    pass "② overload RootSlotWriteback(..., $t)"
  else
    bad "② missing overload RootSlotWriteback(..., $t)"
  fi
done
# nullslot non-heap arm preserved
nh=$(grep -c 'Non-heap targets' runtime/src/Heap/WCollector/WCollector.cpp || true)
if [[ "$nh" == "1" ]]; then
  pass "② Non-heap targets comment/arm count == 1"
else
  bad "② Non-heap targets count=$nh (want 1)"
fi
if grep -A20 'Non-heap targets' runtime/src/Heap/WCollector/WCollector.cpp | grep -q 'never CAS null'; then
  pass "② non-heap arm still never CAS null"
else
  bad "② non-heap arm lost never-CAS-null semantics"
fi

# ③ FixMinor interior uses CasInstallInteriorPlain (not plain RefField construct + CAS)
interior_cas=$(grep -c 'CasInstallInteriorPlain' runtime/src/Heap/WCollector/WCollector.cpp || true)
if [[ "$interior_cas" -ge 3 ]]; then
  pass "③ CasInstallInteriorPlain sites >= 3 (got $interior_cas)"
else
  bad "③ CasInstallInteriorPlain sites=$interior_cas (want >=3)"
fi
# FixMinorEvacuatedSlot(RefField) should not construct plain RefField<>(current/target) for writeback
if grep -n 'FixMinorEvacuatedSlot(RefField' -A80 runtime/src/Heap/WCollector/WCollector.cpp \
    | grep -E 'RefField<>\s+\w+\((current|target|toHost)\)' | grep -v RootSlotWriteback | grep -q .; then
  bad "③ FixMinor still constructs plain RefField for object write-back"
else
  pass "③ FixMinor object path no plain RefField<>(obj) write-back"
fi

# ④ census + inject + assert opt-in remain
for sym in RunPlainCensus InjectPlainHeapWriteOnce NotePlainHeapWrite AssertColouredWriteIfEnabled; do
  if grep -rn "$sym" runtime/src/Heap/Verify/PlainCensus.* runtime/src/Common/BaseObject.cpp >/dev/null 2>&1 \
     || grep -rn "$sym" runtime/src/Heap/Verify/PlainCensus.h runtime/src/Heap/Verify/PlainCensus.cpp runtime/src/Common/BaseObject.cpp >/dev/null; then
    pass "④ symbol present: $sym"
  else
    bad "④ missing symbol: $sym"
  fi
done
if grep -n 'MRT_GCV2_PLAIN_WRITE_INJECT' runtime/src/Heap/Verify/PlainCensus.cpp | grep -q .; then
  pass "④ inject positive control env present"
else
  bad "④ inject positive control missing"
fi
if grep -n 'MRT_GCV2_ASSERT_COLOURED_WRITES' runtime/src/Common/BaseObject.cpp | grep -q .; then
  pass "④ ASSERT_COLOURED_WRITES opt-in present"
else
  bad "④ ASSERT_COLOURED_WRITES missing"
fi
# default-off evidence: env gates require explicit =1
if grep -A3 'AssertColouredWriteIfEnabled' runtime/src/Common/BaseObject.cpp | grep -q "v\[0\] == '1'"; then
  pass "④ ASSERT default-off (requires env=1)"
else
  # broader check
  if grep -n "MRT_GCV2_ASSERT_COLOURED_WRITES" -A5 runtime/src/Common/BaseObject.cpp | grep -q "'1'"; then
    pass "④ ASSERT default-off (requires env=1)"
  else
    bad "④ ASSERT default-off gate unclear"
  fi
fi

echo "----"
if [[ "$fail" -eq 0 ]]; then
  echo "trustp1_static_harness: ALL PASS"
  exit 0
fi
echo "trustp1_static_harness: FAILURES"
exit 1
