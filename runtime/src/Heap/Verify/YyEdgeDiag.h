// ⛔ HOLLOWED — the implementation in the matching .cpp is all no-ops: Enabled() returns false and
// every sink body is empty.  The gate documented below therefore emits nothing, so a zero taken from
// it is a false negative, not evidence that the arm never fires.  The contract, the gate name and the
// product call sites were all left intact when the bodies were removed, which is precisely what makes
// this readable as a live instrument.  Restore the sink you need first -- PermWhoAdmit.cpp shows the
// shape: a compile-time constant gate (the campaign cut MRT_GCV2_* from 190 to 3) plus a line on the
// zero case so a zero cannot be read as a dead probe.  Guard: runtime/tests/check_diag_not_hollow.py
#ifndef MRT_YY_EDGE_DIAG_H
#define MRT_YY_EDGE_DIAG_H

#include <cstddef>
#include <unordered_set>
#include <vector>

#include "Common/BaseObject.h"
#include "Common/TypeDef.h"

namespace MapleRuntime {
namespace YyEdgeDiag {

// Default off.  MRT_GCV2_YYEDGE=1 or MRT_GCV2_DIAG token yyedge.
// Counts young→young write-barrier skips and publishes the product reachableVec
// so H3 can say whether a live holder was in this / previous minor fix set.
bool Enabled();

// Default off.  MRT_GCV2_RECORD_Y2Y=1 records young→young into remset.
// Must stay off unless a measurement closes the cost question.
bool RecordEnabled();

// RecordCrossGenEdge: heap holder in a young region, target young.
void NoteYoungToYoung(BaseObject* holder, MAddress fieldAddress, BaseObject* ref);

// After TraceYoungClosure materialises reachableVec (pre-evacuate).
void PublishProductVec(const std::vector<BaseObject*>& reachableVec);

bool HolderInThisProductVec(BaseObject* holder);
bool HolderInPrevProductVec(BaseObject* holder);

void Report(const char* point);

} // namespace YyEdgeDiag
} // namespace MapleRuntime

#endif
