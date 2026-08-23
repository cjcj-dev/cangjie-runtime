// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_MARK_COMPLETE_VERIFY_H
#define MRT_MARK_COMPLETE_VERIFY_H

// Port of ZVerify::after_mark (zVerify.cpp:496-506).
//
// ZGC reclaims a page on `!is_marked` alone (zGeneration.cpp:216-221,
// zPage.inline.hpp:254-256).  That is only sound because of a guarantee it checks
// directly: once mark claims completion, every reference out of a live old object
// points at a live old object -- ZVerifyOldOopClosure -> z_verify_old_oop's
// `guarantee(ZPointer::is_marked_old(o))` (zVerify.cpp:131-155), driven over the
// whole old generation by ZVerifyObjectClosure::do_object (zVerify.cpp:432-444).
//
// Our tree states in a comment that it does not have that guarantee
// (RegionInfo.h:2765-2769: "register_empty_page iff !is_marked -- but that is safe
// only because ZGC's mark is complete for every relocatable page.  Ours is not"),
// and two lanes have now paid for it: oldroots got an IDLE ForwardObjectImpl UAF and
// oldroots2 got SEGV si_addr=0x8 plus a checksum drift from the same knife.  Neither
// run could name the offending edge, because the crash happens long after the page
// is gone.  This names it at the moment mark declares completion instead.
//
// The verifier answers exactly one question: at old mark end, which (live holder,
// field, target) edges point at an object this cycle's mark did not mark?  That is
// the precondition for `!is_marked`, so a zero here is what licenses the knife and
// a non-zero is the defect list, with holder/field/target/region identity attached.
//
// Deliberately not counted as defects (these are live-by-construction, and
// IsKnownEmpty already refuses to call their regions empty):
//   - targets in young regions            (the minor's mark face owns them; ZGC's
//                                          z_verify_old_oop:150 allows bad young bits)
//   - targets in a region with a mark-start alloc gap (allocate-black; RegionInfo.h:2779)
//   - interior pointers whose recovered base is marked (introot's RawArray+8 shape)
// They are counted in their own columns so a zero in the defect column cannot be
// confused with "the walk never reached anything".
//
// Gate: MRT_GCV2_MARKCOMPLETE=1 or MRT_GCV2_DIAG token "markcomplete".  Default off,
// and the product path early-returns before any counter or STW.
// Fail-closed variant: MRT_GCV2_MARKCOMPLETE_FATAL=1 (report-only otherwise).
// Nested deadInterior census: MRT_GCV2_MARKCOMPLETE_INTEDGE=1 / token "intedge"
// (no-op unless MARKCOMPLETE is already on). Observation only; does not change
// the okInteriorBase exemption or the deadFrom arm.

namespace MapleRuntime {

namespace MarkCompleteVerify {

bool Enabled();

// Runs under its own ScopedStopTheWorld, so it must be called from the GC thread
// outside any existing STW scope.  Report-only unless the FATAL gate is set.
void RunAtMarkEnd(const char* point);

} // namespace MarkCompleteVerify

} // namespace MapleRuntime

#endif // MRT_MARK_COMPLETE_VERIFY_H
