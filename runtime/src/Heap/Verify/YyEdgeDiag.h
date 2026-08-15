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
