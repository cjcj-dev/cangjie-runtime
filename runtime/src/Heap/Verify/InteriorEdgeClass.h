// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_INTERIOR_EDGE_CLASS_H
#define MRT_INTERIOR_EDGE_CLASS_H

#include <cstdint>

// Classifier for MarkCompleteVerify's deadInterior arm.
//
// deadInterior today means: the target failed PlausibleManagedObjectGate and
// TryRecoverInteriorBase did not return a live base (MarkCompleteVerify.cpp).
// That mixes four things the next reader cannot tell apart:
//   - the verifier walked a non-ref word as if it were a reference
//   - a real interior whose live host the 8/16/24/32 heuristic missed
//     (ZGC never guesses: derived and base are paired on the oopmap,
//      zMark.cpp:691-692 ZUncoloredRoot::mark)
//   - a real interior whose host is unmarked (same shape as the deadFrom
//     main case; survnode owns that)
//   - a value that does not sit inside any size-walk object in its region
//
// The four kinds are mutually exclusive and exhaustive for any deadInterior
// edge. Classify is a pure function of those four booleans so gc_unit can
// drive the table without a heap.
//
// Observation only and admitted by the Marking face. Does not change
// okInteriorBase / deadFrom / any product path.

namespace MapleRuntime {
namespace InteriorEdgeClass {

enum class Kind : uint8_t {
    SlotNotRef = 0,  // holder word is not a ref in the TypeInfo / GCTib bitmap
    RecoverFail,     // in an object stream, host live, recover missed or disagreed
    BaseUnmarked,    // in an object stream, host not marked/resurrected
    ValueCorrupt,    // not in any size-walk object in the target region
};

inline const char* KindName(Kind k)
{
    switch (k) {
        case Kind::SlotNotRef:
            return "slotNotRef";
        case Kind::RecoverFail:
            return "recoverFail";
        case Kind::BaseUnmarked:
            return "baseUnmarked";
        case Kind::ValueCorrupt:
            return "valueCorrupt";
    }
    return "?";
}

// slotIsRef: holder field is a reference according to TypeInfo GCTib / array
//            component. ForEachRefField is how the verifier got here;
//            disagreement is the SlotNotRef case (non-ref word walked as ref).
// inStream:  size-walk of the target region found a containing object.
// containLive: that object is marked or resurrected this cycle.
// recoverFound: TryRecoverInteriorBase returned a host distinct from target.
// recoverLive: that host is marked or resurrected this cycle.
//
// Size-walk and the 8/16/24/32 heuristic can disagree (walk truncates; multiple
// tip hits make ClassifyInteriorOffset return 0). Either source of a host is
// enough to reject ValueCorrupt.
// walkTruncated: size-walk broke before covering the target address. Absence
// of a containing object is then unproven, so we refuse ValueCorrupt.
inline Kind Classify(bool slotIsRef, bool inStream, bool containLive, bool recoverFound, bool recoverLive,
                     bool walkTruncated)
{
    if (!slotIsRef) {
        return Kind::SlotNotRef;
    }
    if (inStream) {
        return containLive ? Kind::RecoverFail : Kind::BaseUnmarked;
    }
    if (recoverFound) {
        return recoverLive ? Kind::RecoverFail : Kind::BaseUnmarked;
    }
    if (walkTruncated) {
        return Kind::RecoverFail;
    }
    return Kind::ValueCorrupt;
}

} // namespace InteriorEdgeClass
} // namespace MapleRuntime

#endif // MRT_INTERIOR_EDGE_CLASS_H
