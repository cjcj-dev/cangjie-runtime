// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_REGIONINFO_INLINE_H
#define MRT_REGIONINFO_INLINE_H

#include "Heap/z/zPage.hpp"

namespace MapleRuntime {



inline void RegionInfo::SetMarkFaceSealed(bool v)
    {
        if (v) {
            __atomic_fetch_or(&metadata.markFaceSealed, MARK_FACE_SEALED_BIT, __ATOMIC_RELEASE);
        } else {
            __atomic_fetch_and(&metadata.markFaceSealed, static_cast<uint8_t>(~MARK_FACE_SEALED_BIT),
                               __ATOMIC_RELEASE);
        }
    }

    template<Generation G>
inline uint64_t RegionInfo::GetMarkSnapshotEpoch() const
    {
        (void)G;
        return GetSnapshotEpoch();
    }

    template<Generation G>
inline MarkView<G> RegionInfo::GetMarkView()
    {
        // A major closure visits both young and old regions.  A minor closure is
        // only authoritative for young regions, so minting the inverse binding is
        // rejected at the sole constructor boundary.
        CHECK_DETAIL(G != Generation::Young || IsYoungRegion(),
                     "cannot bind a young mark view to old region %p", this);
        const RegionLifeId life = GetRegionLifeId();
        return MarkView<G>(this, GetMarkSnapshotEpoch<G>(), life);
    }

    template<Generation G>
inline bool RegionInfo::ValidateMarkView(MarkView<G> view) const
    {
        CHECK(view.GetRegion() == this);
        return (view.GetLifeId() == GetRegionLifeId());
    }

inline bool RegionInfo::IsCompacted() const
    {
        auto owner = ForwardingTable::RetainPageOwner(const_cast<RegionInfo*>(this));
        return owner && owner->is_done() && owner->in_place();
    }

inline bool RegionInfo::IsRoutingState()
    {
        auto owner = ForwardingTable::RetainPageOwner(this);
        return owner && owner->is_claimed() && !owner->is_done();
    }





inline RegionInfo* RegionInfo::NullRegion()
    {
        static RegionInfo nullRegion;
        return &nullRegion;
    }

inline LiveInfo* RegionInfo::GetLiveInfo()
    {
        LiveInfo* liveInfo = __atomic_load_n(&metadata.liveInfo, std::memory_order_acquire);
        return liveInfo;
    }

inline LiveInfo* RegionInfo::GetLiveInfo() const
    {
        LiveInfo* liveInfo = __atomic_load_n(&metadata.liveInfo, std::memory_order_acquire);
        return liveInfo;
    }

    template<Generation G>
inline LiveInfo* RegionInfo::GetLiveInfoForView(MarkView<G> view) const
    {
        LiveInfo* current = __atomic_load_n(&metadata.liveInfo, std::memory_order_acquire);
        if (current != nullptr && view.GetEpoch() != 0 && view.GetEpoch() == GetSnapshotEpoch() &&
            current->GetMarkFace().epoch.load(std::memory_order_acquire) == view.GetEpoch()) {
            return current;
        }
        const ZForwarding::FromPageView* from = GetFromPageView();
        if (from != nullptr && from->owner == static_cast<uint8_t>(G) && from->epoch == view.GetEpoch()) {
            return from->liveInfo;
        }
        return nullptr;
    }

inline ZForwarding* RegionInfo::GetFromPageCarrier() const
    {
        ZForwarding* carrier = ForwardingTable::RetainPageOwner(this).get();
        return carrier != nullptr && carrier->page() == this ? carrier : nullptr;
    }

inline bool RegionInfo::HasFromPageMetadata() const
    {
        const ZForwarding::FromPageView* from = GetFromPageView();
        return from != nullptr && (from->lifeId == GetRegionLifeId());
    }



inline Generation RegionInfo::GetRouteMarkGeneration() const
    {
        const ZForwarding::FromPageView* from = GetFromPageView();
        return from == nullptr ? GetOwnerGeneration() : static_cast<Generation>(from->owner);
    }

    template<Generation G>
inline MarkView<G> RegionInfo::GetRouteMarkView()
    {
        CHECK_DETAIL(GetRouteMarkGeneration() == G,
                     "route mark generation mismatch region=%p have=%u want=%u", this,
                     static_cast<unsigned>(GetRouteMarkGeneration()), static_cast<unsigned>(G));
        const ZForwarding::FromPageView* from = GetFromPageView();
        if (from == nullptr) {
            return GetMarkView<G>();
        }
        return MarkView<G>(this, from->epoch, from->lifeId);
    }

    template<Generation G>
inline uint64_t RegionInfo::GetMarkEpoch(MarkView<G> view, LiveInfo* liveInfo) const
    {
        CHECK(view.GetRegion() == this);
        if (!ValidateMarkView(view)) {
            return 0;
        }
        return liveInfo == nullptr ? 0 : liveInfo->GetMarkFace().epoch.load(std::memory_order_acquire);
    }

    template<Generation G>
inline RegionBitmap* RegionInfo::GetMarkBitmap(MarkView<G> view, LiveInfo* liveInfo) const
    {
        CHECK(view.GetRegion() == this);
        if (!ValidateMarkView(view)) {
            return nullptr;
        }
        if (liveInfo == nullptr || view.GetEpoch() == 0 ||
            liveInfo->GetMarkFace().epoch.load(std::memory_order_acquire) != view.GetEpoch()) {
            return nullptr;
        }
        RegionBitmap* bitmap =
            __atomic_load_n(&liveInfo->GetMarkFace().bitmap, std::memory_order_acquire);
        return bitmap;
    }

    template<Generation G>
inline bool RegionInfo::IsSurvivedObject(MarkView<G> view, LiveInfo* liveInfo, size_t offset) const
    {
        CHECK(view.GetRegion() == this);
        if (!ValidateMarkView(view)) {
            return false;
        }
        return liveInfo != nullptr && liveInfo->IsSurvivedObject(view, offset);
    }

inline bool RegionInfo::FromPageAllocatedAfterMarkStart(size_t offset) const
    {
        const ZForwarding::FromPageView* from = GetFromPageView();
        return from != nullptr && from->markStartAllocPtr != 0 &&
            GetRegionStart() + offset >= from->markStartAllocPtr;
    }

inline bool RegionInfo::HasFromPageMarkStartAllocGap() const
    {
        const ZForwarding::FromPageView* from = GetFromPageView();
        return from != nullptr && from->markStartAllocPtr != 0 &&
            from->topAtStart > from->markStartAllocPtr;
    }

    template<Generation G>
inline bool RegionInfo::IsFromPageSurvivedObject(MarkView<G> view, size_t offset) const
    {
        const ZForwarding::FromPageView* from = GetFromPageView();
        if (from == nullptr) {
            return false;
        }
        if (IsLargeRegion()) {
            return from->largeMarked != 0 || FromPageAllocatedAfterMarkStart(offset);
        }
        return IsSurvivedObject(view, from->liveInfo, offset) || FromPageAllocatedAfterMarkStart(offset);
    }

inline bool RegionInfo::IsRouteSurvivedObject(size_t offset)
    {
        if (!HasFromPageMetadata()) {
            if (IsYoungRegion()) {
                MarkView<Generation::Young> view = GetMarkView<Generation::Young>();
                return IsSurvivedObject(view, GetLiveInfo(), offset) || AllocatedAfterMarkStart(offset);
            }
            MarkView<Generation::Old> view = GetMarkView<Generation::Old>();
            return IsSurvivedObject(view, GetLiveInfo(), offset) || AllocatedAfterMarkStart(offset);
        }
        if (GetRouteMarkGeneration() == Generation::Young) {
            MarkView<Generation::Young> view = GetRouteMarkView<Generation::Young>();
            if (!ValidateMarkView(view)) {
                return false;
            }
            return IsFromPageSurvivedObject(view, offset);
        }
        MarkView<Generation::Old> view = GetRouteMarkView<Generation::Old>();
        if (!ValidateMarkView(view)) {
            return false;
        }
        return IsFromPageSurvivedObject(view, offset);
    }

inline bool RegionInfo::IsRouteMarkedObject(size_t offset)
    {
        const ZForwarding::FromPageView* from = GetFromPageView();
        if (from == nullptr) {
            if (IsYoungRegion()) {
                return IsMarkedObject(GetMarkView<Generation::Young>(), offset);
            }
            return IsMarkedObject(GetMarkView<Generation::Old>(), offset);
        }
        if (GetRouteMarkGeneration() == Generation::Young) {
            MarkView<Generation::Young> view = GetRouteMarkView<Generation::Young>();
            if (!ValidateMarkView(view)) {
                return false;
            }
            if (FromPageAllocatedAfterMarkStart(offset)) {
                return true;
            }
            if (IsLargeRegion()) {
                return from->largeMarked != 0;
            }
            RegionBitmap* bitmap = GetMarkBitmap(view, from->liveInfo);
            return bitmap != nullptr && bitmap->IsMarked(offset);
        }
        MarkView<Generation::Old> view = GetRouteMarkView<Generation::Old>();
        if (!ValidateMarkView(view)) {
            return false;
        }
        if (FromPageAllocatedAfterMarkStart(offset)) {
            return true;
        }
        if (IsLargeRegion()) {
            return from->largeMarked != 0;
        }
        RegionBitmap* bitmap = GetMarkBitmap(view, from->liveInfo);
        return bitmap != nullptr && bitmap->IsMarked(offset);
    }

inline bool RegionInfo::IsRouteKnownEmpty()
    {
        const ZForwarding::FromPageView* from = GetFromPageView();
        if (from == nullptr) {
            if (IsYoungRegion()) {
                return IsKnownYoungEmpty(GetMarkView<Generation::Young>());
            }
            return IsKnownEmpty(GetMarkView<Generation::Old>());
        }
        if (HasFromPageMarkStartAllocGap()) {
            return false;
        }
        RegionBitmap* bitmap = GetRouteMarkBitmap(from->liveInfo);
        return bitmap != nullptr && bitmap->GetLiveBytes() == 0;
    }

inline RegionBitmap* RegionInfo::GetRouteMarkBitmap(LiveInfo* face)
    {
        const ZForwarding::FromPageView* from = GetFromPageView();
        LiveInfo* selected = face != nullptr ? face
            : (from != nullptr ? from->liveInfo : GetLiveInfo());
        if (GetRouteMarkGeneration() == Generation::Young) {
            return GetMarkBitmap(GetRouteMarkView<Generation::Young>(), selected);
        }
        return GetMarkBitmap(GetRouteMarkView<Generation::Old>(), selected);
    }

inline uint64_t RegionInfo::GetRouteMarkEpoch(LiveInfo* face)
    {
        if (GetRouteMarkGeneration() == Generation::Young) {
            return GetMarkEpoch(GetRouteMarkView<Generation::Young>(), face);
        }
        return GetMarkEpoch(GetRouteMarkView<Generation::Old>(), face);
    }

inline uint64_t RegionInfo::GetRouteMarkSnapshotEpoch() const
    {
        const ZForwarding::FromPageView* from = GetFromPageView();
        return from == nullptr ? GetSnapshotEpoch() : from->epoch;
    }

inline size_t RegionInfo::RecomputeRouteBitmapLiveBytes(LiveInfo* face)
    {
        if (face == nullptr) {
            return 0;
        }
        if (GetRouteMarkGeneration() == Generation::Young) {
            return face->RecomputeBitmapLiveBytes(GetRouteMarkView<Generation::Young>());
        }
        return face->RecomputeBitmapLiveBytes(GetRouteMarkView<Generation::Old>());
    }

inline size_t RegionInfo::GetRouteBitmapLiveBytes(LiveInfo* face)
    {
        if (face == nullptr) {
            return 0;
        }
        if (GetRouteMarkGeneration() == Generation::Young) {
            MarkView<Generation::Young> view = GetRouteMarkView<Generation::Young>();
            return face->GetBitmapLiveBytes(view);
        }
        MarkView<Generation::Old> view = GetRouteMarkView<Generation::Old>();
        return face->GetBitmapLiveBytes(view);
    }

inline void RegionInfo::BindLiveInfo0FromLiveIfNull()
    {
        if (GetLiveInfo0ForProbe() != nullptr) {
            return;
        }
        LiveInfo* live = GetLiveInfo();
        if (live == nullptr) {
            return;
        }
        const uint64_t epoch = live->GetMarkFace().epoch.load(std::memory_order_acquire);
        const RegionLifeId life = GetRegionLifeId();
        CHECK_DETAIL(ForwardingTable::PublishFromPageView(
                         this, live, epoch, GetRegionAllocPtr(), metadata.markStartAllocPtr,
                         static_cast<uint8_t>(GetOwnerGeneration()),
                         static_cast<uint8_t>((IsLargeRegion() ? IsCurrentFacePublished() : metadata.isMarked != 0) ||
                                              metadata.isResurrected != 0),
                         life),
                     "from-page forwarding carrier missing while binding live face region=%p", this);
    }

inline bool RegionInfo::IsRetainedLifeCurrent() const
    {
        const RegionLifeId stamp = __atomic_load_n(&metadata.retainedLifeId, __ATOMIC_ACQUIRE);
        const RegionLifeId current = GetRegionLifeId();
        const bool auditAccepts =
            (stamp == current);
        // Validate is audit-only unless enforcement is enabled and therefore
        // deliberately accepts missing/stale stamps in ordinary product runs.
        // Snapshot-state derivation needs the structural answer in every
        // configuration: zero is not a carrier, and only this life is current.
        return stamp != 0 && stamp == current && auditAccepts;
    }

inline RegionInfo::RetainedLiveInfoState RegionInfo::GetRetainedLiveInfoState() const
    {
        if (metadata.retainedEverPreserved == 0) {
            return RetainedLiveInfoState::NEVER_EXAMINED;
        }
        if (!IsRetainedLifeCurrent()) {
            return RetainedLiveInfoState::SNAPSHOT_LOST;
        }
        return metadata.retainedLiveInfoCoveredUpTo <= GetRegionStart() &&
                GetRegionAllocPtr() <= GetRegionStart()
            ? RetainedLiveInfoState::SNAPSHOT_EMPTY
            : RetainedLiveInfoState::SNAPSHOT_VALID;
    }

inline void RegionInfo::StampRetainedSnapshot()
    {
        const RegionLifeId life = GetRegionLifeId();
        __atomic_store_n(&metadata.retainedLifeId, life, __ATOMIC_RELEASE);
    }

inline void RegionInfo::CaptureRetainedMarkWords(LiveInfo* liveInfo, uint64_t epoch, uint8_t largeMarked)
    {
        FreeRetainedMarkWords();
        if (IsLargeRegion()) {
            // ZGC large pages contain one object at page start (zPage.inline.hpp:53-58),
            // but still represent its liveness with the page livemap (228-240). Mirror
            // our large-region mark/resurrect single bits in retained word bit zero.
            bool marked = largeMarked != 0 || metadata.isResurrected == 1;
            if (!marked) {
                return;
            }
            uint64_t* words = static_cast<uint64_t*>(malloc(sizeof(uint64_t)));
            CHECK(words != nullptr);
            words[0] = 1;
            metadata.retainedMarkWords = words;
            metadata.retainedMarkWordCnt = 1;
            return;
        }
        if (liveInfo == nullptr) {
            return;
        }
        LiveInfo::MarkFace& markFace = liveInfo->GetMarkFace();
        RegionBitmap* mark = markFace.epoch.load(std::memory_order_acquire) == epoch
            ? __atomic_load_n(&markFace.bitmap, std::memory_order_acquire) : nullptr;
        RegionBitmap* resurrect = liveInfo->resurrectBitmap;
        size_t markWords = mark == nullptr ? 0 : mark->wordCnt.load(std::memory_order_acquire);
        size_t resurrectWords = resurrect == nullptr ? 0 : resurrect->wordCnt.load(std::memory_order_acquire);
        size_t wordCnt = std::max(markWords, resurrectWords);
        if (wordCnt == 0) {
            return;
        }
        uint64_t* words = static_cast<uint64_t*>(malloc(wordCnt * sizeof(uint64_t)));
        CHECK(words != nullptr);
        for (size_t i = 0; i < wordCnt; ++i) {
            uint64_t bits = 0;
            if (i < markWords) {
                bits |= mark->GetLiveWord(i);
            }
            if (i < resurrectWords) {
                bits |= resurrect->GetLiveWord(i);
            }
            words[i] = bits;
        }
        metadata.retainedMarkWords = words;
        metadata.retainedMarkWordCnt = static_cast<uint32_t>(wordCnt);
    }

inline bool RegionInfo::RetainedMarkWordsSay(size_t offset) const
    {
        if (!IsRetainedLifeCurrent()) {
            return false;
        }
        if (metadata.retainedMarkWords == nullptr) {
            return false;
        }
        size_t bitIdx = 2 * (offset / kMarkedBytesPerBit);
        size_t wordIdx = bitIdx / kBitsPerWord;
        if (wordIdx >= metadata.retainedMarkWordCnt) {
            return false;
        }
        return (metadata.retainedMarkWords[wordIdx] &
                (static_cast<uint64_t>(1) << (bitIdx % kBitsPerWord))) != 0;
    }

inline void RegionInfo::FreeRetainedMarkWords()
    {
        if (metadata.retainedMarkWords != nullptr) {
            free(metadata.retainedMarkWords);
            metadata.retainedMarkWords = nullptr;
        }
        metadata.retainedMarkWordCnt = 0;
    }

inline uint32_t RegionInfo::GetRetainedPreserveCount() const
    {
        return __atomic_load_n(&metadata.retainedPreserveCnt, __ATOMIC_ACQUIRE) &
            ~FORWARDING_FACE_RESET_BIT;
    }

inline void RegionInfo::PreserveRetainedLiveInfo()
    {
        BeginRetainedPreserve();
        metadata.retainedLiveInfo = GetLiveInfo();
        metadata.retainedLiveInfoEpoch = GetSnapshotEpoch();
        const bool largeRegion = IsLargeRegion();
        const uint64_t largeLiveBytes = largeRegion ? GetLiveByteCount() : 0;
        uint8_t largeMarked = largeRegion ? IsCurrentFacePublished() : metadata.isMarked;
        const ZForwarding::FromPageView* from = GetFromPageView();
        if (metadata.retainedLiveInfo == nullptr && from != nullptr) {
            metadata.retainedLiveInfo = from->liveInfo;
            metadata.retainedLiveInfoEpoch = from->epoch;
            largeMarked = from->largeMarked;
        }
        // A done bit may outlive the face it described.  Suppress the young
        // face while it is still the forwarding face, but keep a later face
        // published by the current snapshot (ZGC's page-face identity rule).
        if (IsYoungRegion() && IsForwardingDone() &&
            (!IsCurrentFacePublished() || IsForwardingFaceCurrent())) {
            metadata.retainedLiveInfo = nullptr;
            largeMarked = 0;
        }
        metadata.retainedLiveInfoCoveredUpTo = GetRegionAllocPtr();
        if (RetainedOwnCopyEnabled()) {
            CaptureRetainedMarkWords(metadata.retainedLiveInfo, metadata.retainedLiveInfoEpoch, largeMarked);
        }
        if (IsLargeRegion()) {
            // A stale byte count without the publication bit belongs to the
            // retired face (for example while ClearLiveInfo is sealing it),
            // never to a current valid carrier.
            if (largeLiveBytes == 0 || largeMarked == 0) {
                NoteRetainedPreserve(GetRegionAllocPtr() <= GetRegionStart());
                return;
            }
            NoteRetainedPreserve(true);
            return;
        }
        if (metadata.retainedLiveInfo != nullptr) {
            NoteRetainedPreserve(true);
            return;
        }
        CHECK(GetLiveByteCount() == 0);
        NoteRetainedPreserve(GetRegionAllocPtr() <= GetRegionStart());
    }

inline void RegionInfo::StampCensusBoundary()
    {
        uintptr_t offset = GetRegionAllocPtr() - GetRegionStart();
        metadata.censusBoundaryOffset =
            static_cast<uint32_t>(std::min<uintptr_t>(offset, std::numeric_limits<uint32_t>::max()));
    }

inline void RegionInfo::PreserveRetainedLiveInfoUpTo(MAddress boundary)
    {
        CHECK(boundary >= GetRegionStart() && boundary <= GetRegionAllocPtr());
        if (IsLargeRegion()) {
            PreserveRetainedLiveInfo();
            return;
        }
        BeginRetainedPreserve();
        metadata.retainedLiveInfo = GetLiveInfo();
        metadata.retainedLiveInfoEpoch = GetSnapshotEpoch();
        uint8_t largeMarked = IsLargeRegion() ? IsCurrentFacePublished() : metadata.isMarked;
        const ZForwarding::FromPageView* from = GetFromPageView();
        if (metadata.retainedLiveInfo == nullptr && from != nullptr) {
            metadata.retainedLiveInfo = from->liveInfo;
            metadata.retainedLiveInfoEpoch = from->epoch;
            largeMarked = from->largeMarked;
        }
        if (IsYoungRegion() && IsForwardingDone() &&
            (!IsCurrentFacePublished() || IsForwardingFaceCurrent())) {
            metadata.retainedLiveInfo = nullptr;
            largeMarked = 0;
        }
        metadata.retainedLiveInfoCoveredUpTo = boundary;
        if (RetainedOwnCopyEnabled()) {
            CaptureRetainedMarkWords(metadata.retainedLiveInfo, metadata.retainedLiveInfoEpoch, largeMarked);
        }
        if (metadata.retainedLiveInfo == nullptr) {
            // This decision is derived after CaptureRetainedMarkWords has
            // replaced the previous owned carrier.  Once a successful
            // Preserve armed the monotonic bit, carrier absence is LOST on
            // every exit; no clear/unbind exit has to remember to write it.
            CHECK(GetRetainedLiveInfoState() != RetainedLiveInfoState::SNAPSHOT_LOST);
            NoteRetainedPreserve(false);
            return;
        }
        NoteRetainedPreserve(true);
    }

inline ALWAYS_INLINE void RegionInfo::PreserveRetainedLiveInfo(MAddress coveredUpToOverride)
    {
        if (coveredUpToOverride == GetRegionStart() && GetRegionAllocPtr() != GetRegionStart()) {
            CHECK(GetLiveByteCount() == 0);
            BeginRetainedPreserve();
            metadata.retainedLiveInfo = GetLiveInfo();
            metadata.retainedLiveInfoEpoch = GetSnapshotEpoch();
            metadata.retainedLiveInfoCoveredUpTo = coveredUpToOverride;
            NoteRetainedPreserve(true);
            return;
        }
        CHECK(coveredUpToOverride == GetRegionAllocPtr());
        PreserveRetainedLiveInfo();
    }

inline ALWAYS_INLINE void RegionInfo::NoteRetainedPreserve(bool succeeded)
    {
        // The high bits share this word with first-paint publication. Marking
        // may publish from multiple workers, so keep the counter increment in
        // the same atomic modification order instead of losing either flag.
        (void)__atomic_fetch_add(&metadata.retainedPreserveCnt, 1U, __ATOMIC_ACQ_REL);
        if (succeeded) {
            metadata.retainedEverPreserved = 1;
            // Publish last: an acquiring reader that accepts this life also
            // observes the retained pointer/owned words and covered boundary.
            StampRetainedSnapshot();
        }
        switch (GetRetainedLiveInfoState()) {
            case RetainedLiveInfoState::SNAPSHOT_VALID:
                metadata.retainedLastOp = RETAINED_OP_PRESERVE_VALID;
                break;
            case RetainedLiveInfoState::SNAPSHOT_EMPTY:
                metadata.retainedLastOp = RETAINED_OP_PRESERVE_EMPTY;
                break;
            default:
                metadata.retainedLastOp = RETAINED_OP_PRESERVE_NEVER;
                break;
        }
    }

inline ALWAYS_INLINE void RegionInfo::NoteRetainedClear(RetainedOp op)
    {
        if (GetRetainedLiveInfoState() == RetainedLiveInfoState::NEVER_EXAMINED) {
            return;
        }
        ++metadata.retainedClearCnt;
        metadata.retainedLastOp = static_cast<uint8_t>(op);
    }

inline bool RegionInfo::IsRetainedSnapshotValid() const
    {
        RetainedLiveInfoState state = GetRetainedLiveInfoState();
        if (state == RetainedLiveInfoState::NEVER_EXAMINED ||
            state == RetainedLiveInfoState::SNAPSHOT_LOST) {
            return false;
        }
        if (!IsRetainedLifeCurrent()) {
            return false;
        }
        // An owned copy is the persistent livemap carrier. Its lifetime is
        // ended explicitly by ClearLiveInfo<Old> or region reinitialization;
        // forwarding's epoch bump only retires the borrowed LiveInfo face.
        if (metadata.retainedMarkWords != nullptr) {
            return true;
        }
        return metadata.retainedLiveInfoEpoch == GetSnapshotEpoch();
    }

inline void RegionInfo::InitializeLiveInfo()
    {
        LiveInfo* live = LiveInfoArena::GetLiveInfoArena().AllocateLiveInfo(this);
        live->bindedRegion = this;
        live->resurrectBitmap = LiveInfoArena::GetLiveInfoArena().AllocateRegionBitmap(GetRegionSize());
        live->enqueueBitmap = LiveInfoArena::GetLiveInfoArena().AllocateRegionBitmap(GetRegionSize());
        __atomic_store_n(&metadata.liveInfo, live, std::memory_order_release);
    }

    template<Generation G>
inline RegionBitmap* RegionInfo::GetMarkBitmap(MarkView<G> view)
    {
        CHECK(view.GetRegion() == this);
        if (!ValidateMarkView(view)) {
            return nullptr;
        }
        LiveInfo* liveInfo = GetLiveInfoForView(view);
        if (liveInfo == nullptr) {
            return nullptr;
        }
        LiveInfo::MarkFace& face = liveInfo->GetMarkFace();
        if (face.epoch.load(std::memory_order_acquire) != view.GetEpoch()) {
            return nullptr;
        }
        RegionBitmap* bitmap = __atomic_load_n(&face.bitmap, std::memory_order_acquire);
        return bitmap;
    }

    template<Generation G>
inline RegionBitmap* RegionInfo::GetOrAllocMarkBitmap(MarkView<G> view)
    {
        CHECK(view.GetRegion() == this);
        CHECK(view.GetEpoch() != 0 && view.GetEpoch() == GetMarkSnapshotEpoch<G>());
        LiveInfo* liveInfo = GetLiveInfo();
        CHECK(liveInfo != nullptr);
        LiveInfo::MarkFace& face = liveInfo->GetMarkFace();
        constexpr uint64_t kInitializing = std::numeric_limits<uint64_t>::max();
        for (;;) {
            uint64_t seq = face.epoch.load(std::memory_order_acquire);
            if (seq == view.GetEpoch()) {
                return __atomic_load_n(&face.bitmap, std::memory_order_acquire);
            }
            if (seq != kInitializing &&
                face.epoch.compare_exchange_strong(seq, kInitializing, std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
                RegionBitmap* bitmap = __atomic_load_n(&face.bitmap, std::memory_order_relaxed);
                if (bitmap == nullptr) {
                    bitmap = LiveInfoArena::GetLiveInfoArena().AllocateRegionBitmap(GetRegionSize());
                    __atomic_store_n(&face.bitmap, bitmap, std::memory_order_relaxed);
                } else if (!bitmap->CoversRegionSize(GetRegionSize())) {
                    bitmap = LiveInfoArena::GetLiveInfoArena().PublishMatchingBitmap(
                        &face.bitmap, bitmap, GetRegionSize(), liveInfo);
                }
                bitmap->Reset();
                // ZLiveMap::reset: publish only after allocation and metadata reset.
                face.epoch.store(view.GetEpoch(), std::memory_order_release);
                return bitmap;
            }
            sched_yield();
        }
    }

inline RegionBitmap* RegionInfo::GetResurrectBitmap()
    {
        LiveInfo* liveInfo = GetLiveInfo();
        if (liveInfo == nullptr) {
            return nullptr;
        }
        RegionBitmap* bitmap = __atomic_load_n(&liveInfo->resurrectBitmap, std::memory_order_acquire);
        return bitmap;
    }

inline RegionBitmap* RegionInfo::GetEnqueueBitmap()
    {
        LiveInfo* liveInfo = GetLiveInfo();
        if (liveInfo == nullptr) {
            return nullptr;
        }
        RegionBitmap* bitmap = __atomic_load_n(&liveInfo->enqueueBitmap, std::memory_order_acquire);
        return bitmap;
    }

    template<Generation G>
inline uint8_t RegionInfo::GetMarkedRegionFlag(MarkView<G> view) const
    {
        CHECK(view.GetRegion() == this);
        if (!ValidateMarkView(view)) {
            return 0;
        }
        // Large pages use one atomic face sequence as both their liveness bit and
        // publication marker. A captured view from an earlier metadata incarnation
        // must not observe a later incarnation's reused bit.
        if (view.GetEpoch() != GetMarkSnapshotEpoch<G>()) {
            const ZForwarding::FromPageView* from = GetFromPageView();
            return from != nullptr && from->owner == static_cast<uint8_t>(G) &&
                from->epoch == view.GetEpoch() ? from->largeMarked : 0;
        }
        if (IsLargeRegion()) {
            return IsCurrentFacePublished() ? 1 : 0;
        }
        return metadata.regionStateBitField.GetAtomicValue(RegionStateBitPos::MARKED_REGION_FLAG, 1) != 0;
    }

    template<Generation G>
inline void RegionInfo::SetMarkedRegionFlag(MarkView<G> view, uint8_t flag)
    {
        CHECK(view.GetRegion() == this);
        CHECK(view.GetEpoch() == GetMarkSnapshotEpoch<G>());
        if (IsLargeRegion()) {
            if (flag != 0) {
                (void)GetOrAllocMarkBitmap(view);
            } else {
                ClearCurrentMarkFace();
            }
            return;
        }
        metadata.regionStateBitField.SetAtomicValue(RegionStateBitPos::MARKED_REGION_FLAG, 1, flag);
    }

inline void RegionInfo::ResetMarkBit(MarkView<Generation::Old> view)
    {
        SurvNodeDiag::NoteClear(this, SurvNodeDiag::CLEAR_RESET_MARK_BIT, false);
        // CollectLargeGarbage calls this for a live large page immediately
        // after mark. Preserve its one-object livemap before clearing the
        // current face, just as ZPage keeps its live bit through relocation.
        if (IsLargeRegion() && IsSurvivedObject(view, 0)) {
            PreserveRetainedLiveInfo();
        }
        SetMarkedRegionFlag(view, 0);
        SetEnqueuedRegionFlag(0);
        SetResurrectedRegionFlag(0);
    }











    template<Generation G>
inline void RegionInfo::VerifyMarkFaceOwner(const BaseObject* obj, const char* site) const
    {
        EnsurePageOwnerVerifyAtexit();
        if (LIKELY(MarkFaceMatchesOwner<G>())) {
            return;
        }
        const size_t mismatch = PageOwnerMismatchAttempts().fetch_add(1, std::memory_order_relaxed) + 1;
        if (PageOwnerVerifyCountOnly()) {
            if (mismatch <= 8) {
                std::fprintf(stderr,
                             "[GCV2][page-owner] mismatch n=%zu site=%s object=%p region=%p owner=%s face=%s\n",
                             mismatch, site, obj, this,
                             GetOwnerGeneration() == Generation::Young ? "young" : "old",
                             G == Generation::Young ? "young" : "old");
                std::fflush(stderr);
            }
            return;
        }
        CHECK_DETAIL(false, "mark face does not match page owner site=%s object=%p region=%p owner=%s face=%s",
                     site, obj, this, GetOwnerGeneration() == Generation::Young ? "young" : "old",
                     G == Generation::Young ? "young" : "old");
    }


    template<Generation G>
inline bool RegionInfo::MarkLargeObject(MarkView<G> view, const BaseObject* obj, size_t size, bool accountLive, bool& firstLive)
    {
        firstLive = false;
        MAddress regionStart = GetRegionStart();
        MAddress regionEnd = GetRegionEnd();
        CheckObjectSize(obj, size, regionStart, regionEnd);
        const size_t offset = reinterpret_cast<MAddress>(obj) - regionStart;
        const size_t regionSize = regionEnd - regionStart;
        RegionBitmap* writeBm = GetOrAllocMarkBitmap(view);
        bool incLive = false;
        const bool already = writeBm->MarkBits(offset, size, regionSize, incLive);
        firstLive = incLive;
        if (incLive) {
            if (accountLive) {
                AddLiveCounts(1, size);
            }
            NotePageOwnerFirstPaint<G>();
        }
        return already;
    }

    template<Generation G>
inline bool RegionInfo::MarkObject(MarkView<G> view, const BaseObject* obj)
    {
        CHECK(view.GetRegion() == this);
        if (!PlausibleManagedObjectGate("RegionInfo::MarkObject.unsized", const_cast<BaseObject*>(obj))) {
            // Rejected objects are deliberately reported as already marked: callers must not
            // enqueue/scan them. If the gate ever rejects a real object, its liveness and
            // transitive reference closure are the work lost by this fail-closed branch.
            return true;
        }
        VerifyMarkFaceOwner<G>(obj, "RegionInfo::MarkObject.unsized");
        if (IsLargeRegion()) {
            bool firstLive = false;
            return MarkLargeObject(view, obj, obj->GetSize(), true, firstLive);
        }
        U32 objSize = obj->GetSize();
        MAddress objAddr = reinterpret_cast<MAddress>(obj);
        MAddress regionStart = GetRegionStart();
        MAddress regionEnd = GetRegionEnd();
        CheckObjectSize(obj, objSize, regionStart, regionEnd);
        size_t offset = objAddr - regionStart;
        size_t regionSize = regionEnd - regionStart;
        RegionBitmap* writeBm = GetOrAllocMarkBitmap(view);

        bool incLive = false;
        bool already = writeBm->MarkBits(offset, objSize, regionSize, incLive);
        if (incLive) {
            AddLiveCounts(1, objSize);
            NotePageOwnerFirstPaint<G>();
        }
        CHECK(IsMarkedObject(view, offset));
        return already;
    }

    template<Generation G>
inline bool RegionInfo::MarkObject(MarkView<G> view, const BaseObject* obj, size_t objSize, bool accountLive)
    {
        bool firstLive = false;
        return MarkObjectWithLiveClaim(view, obj, objSize, accountLive, firstLive);
    }

    template<Generation G>
inline bool RegionInfo::MarkObjectWithLiveClaim(MarkView<G> view, const BaseObject* obj, size_t objSize,
                                 bool accountLive, bool& firstLive)
    {
        firstLive = false;
        CHECK(view.GetRegion() == this);
        VerifyMarkFaceOwner<G>(obj, "RegionInfo::MarkObject.sized");
        if (IsLargeRegion()) {
            return MarkLargeObject(view, obj, objSize, accountLive, firstLive);
        }
        MAddress objAddr = reinterpret_cast<MAddress>(obj);
        MAddress regionStart = GetRegionStart();
        MAddress regionEnd = GetRegionEnd();
        CheckObjectSize(obj, objSize, regionStart, regionEnd);
        size_t offset = objAddr - regionStart;
        size_t regionSize = regionEnd - regionStart;
        RegionBitmap* writeBm = GetOrAllocMarkBitmap(view);

        bool incLive = false;
        bool already = writeBm->MarkBits(offset, objSize, regionSize, incLive);
        firstLive = incLive;
        if (incLive) {
            if (accountLive) {
                AddLiveCounts(1, objSize);
            }
            NotePageOwnerFirstPaint<G>();
        }
        CHECK(IsMarkedObject(view, offset));
        return already;
    }

inline bool RegionInfo::MarkObjectByOwner(const BaseObject* obj)
    {
        if (IsYoungRegion()) {
            return MarkObject(GetMarkView<Generation::Young>(), obj);
        }
        return MarkObject(GetMarkView<Generation::Old>(), obj);
    }

inline bool RegionInfo::MarkObjectByOwner(const BaseObject* obj, size_t objSize, bool accountLive)
    {
        if (IsYoungRegion()) {
            return MarkObject(GetMarkView<Generation::Young>(), obj, objSize, accountLive);
        }
        return MarkObject(GetMarkView<Generation::Old>(), obj, objSize, accountLive);
    }

inline bool RegionInfo::MarkObjectByOwnerWithLiveClaim(const BaseObject* obj, size_t objSize,
                                        bool accountLive, bool& firstLive)
    {
        if (IsYoungRegion()) {
            return MarkObjectWithLiveClaim(GetMarkView<Generation::Young>(), obj, objSize, accountLive, firstLive);
        }
        return MarkObjectWithLiveClaim(GetMarkView<Generation::Old>(), obj, objSize, accountLive, firstLive);
    }

inline bool RegionInfo::ResurrectObject(const BaseObject* obj, size_t offset)
    {
        bool firstLive = false;
        return ResurrectObjectWithLiveClaim(obj, offset, true, firstLive);
    }

inline bool RegionInfo::ResurrectObjectWithLiveClaim(const BaseObject* obj, size_t offset,
                                     bool accountLive, bool& firstLive)
    {
        firstLive = false;
        U32 objSize = obj->GetSize();
        MAddress regionStart = GetRegionStart();
        MAddress regionEnd = GetRegionEnd();
        CheckObjectSize(obj, objSize, regionStart, regionEnd);
        size_t regionSize = regionEnd - regionStart;
        MarkView<Generation::Old> view = GetMarkView<Generation::Old>();
        RegionBitmap* bitmap = GetOrAllocMarkBitmap(view);
        bool incLive = false;
        bool already = bitmap->MarkFinalizableBits(offset, objSize, regionSize, incLive);
        firstLive = incLive;
        if (incLive) {
            if (accountLive) {
                AddLiveCounts(1, objSize);
            }
        }
        CHECK(bitmap->IsLive(offset));
        return already;
    }

inline bool RegionInfo::EnqueueObject(const BaseObject* obj, size_t offset)
    {
        if (IsFreeRegion() || IsGarbageRegion() || GetRegionType() == RegionType::FREE_REGION) {
            return true;
        }
        if (!PlausibleManagedObjectGate("RegionInfo::EnqueueObject", const_cast<BaseObject*>(obj))) {
            // Rejected objects are reported as already enqueued: ShouldEnqueue
            // treats true as "do not SATB-push". Lost work is the SATB entry and
            // the later mark/trace of this address; if the gate ever rejects a
            // real object, that object's SATB-driven liveness is the miss.
            return true;
        }
        if (IsLargeRegion()) {
            if (metadata.isEnqueued != 1) {
                SetEnqueuedRegionFlag(1);
                return false;
            }
            return true;
        }
        U32 objSize = obj->GetSize();
        MAddress regionStart = GetRegionStart();
        MAddress regionEnd = GetRegionEnd();
        CheckObjectSize(obj, objSize, regionStart, regionEnd);
        size_t regionSize = regionEnd - regionStart;
        CHECK(regionSize > 0);
        RegionBitmap* bitmap = GetEnqueueBitmap();
        // enqueue face is not the route geometry face; still report if mark-face sealed.

        bool marked = bitmap->MarkBits(offset, objSize, regionSize);
        CHECK(bitmap->IsMarked(offset));
        return marked;
    }

inline bool RegionInfo::IsResurrectedObject(const BaseObject* obj)
    {
        RegionBitmap* bitmap = GetMarkBitmap(GetMarkView<Generation::Old>());
        if (bitmap == nullptr) {
            return false;
        }
        size_t offset = GetAddressOffset(reinterpret_cast<MAddress>(obj));
        return bitmap->IsFinalizable(offset);
    }

inline bool RegionInfo::IsResurrectedObject(size_t offset)
    {
        RegionBitmap* bitmap = GetMarkBitmap(GetMarkView<Generation::Old>());
        if (bitmap == nullptr) {
            return false;
        }
        return bitmap->IsFinalizable(offset);
    }





    template<Generation G>
inline bool RegionInfo::NoteMarkEpochOnRead(MarkView<G> view, LiveInfo* liveInfo)
    {
        CHECK(view.GetRegion() == this);
        if (!ValidateMarkView(view)) {
            return false;
        }
        if (liveInfo == nullptr) {
            return false;
        }
        LiveInfo::MarkFace& markFace = liveInfo->GetMarkFace();
        RegionBitmap* bitmap = __atomic_load_n(&markFace.bitmap, std::memory_order_acquire);
        // Absence is ordinary "unmarked", not a stale-livemap read.
        if (bitmap == nullptr) {
            return false;
        }
        const uint64_t face = markFace.epoch.load(std::memory_order_acquire);
        const uint64_t now = GetMarkSnapshotEpoch<G>();
        const ZForwarding::FromPageView* from = GetFromPageView();
        const bool currentOrFrom = view.GetEpoch() == now ||
            (from != nullptr && from->owner == static_cast<uint8_t>(G) && from->epoch == view.GetEpoch());
        if (currentOrFrom && face == view.GetEpoch()) {
            return true;
        }
        EnsureMarkEpochAtexit();
        size_t n = markEpochStaleReadCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 8) {
            LOG(RTLOG_ERROR,
                "[GCV2][mark-epoch] stale_read generation=%s region=%p "
                "viewEpoch=%llu faceEpoch=%llu regionEpoch=%llu n=%zu",
                G == Generation::Young ? "young" : "old", this,
                static_cast<unsigned long long>(view.GetEpoch()), static_cast<unsigned long long>(face),
                static_cast<unsigned long long>(now), n);
        }
        return false;
    }

    template<Generation G>
inline bool RegionInfo::IsMarkedObject(MarkView<G> view, const BaseObject* obj)
    {
        CHECK(view.GetRegion() == this);
        if (!ValidateMarkView(view)) {
            return false;
        }
        size_t offset = GetAddressOffset(reinterpret_cast<MAddress>(obj));
        if (view.GetEpoch() == GetMarkSnapshotEpoch<G>() && AllocatedAfterMarkStart(offset)) {
            return true;
        }
        LiveInfo* liveInfo = GetLiveInfoForView(view);
        if (liveInfo == nullptr) {
            return false;
        }
        // markepoch §5: stale face ⇒ unmarked (ZGC is_marked false before bit test).
        if (!NoteMarkEpochOnRead(view, liveInfo)) {
            return false;
        }
        RegionBitmap* markBitmap =
            __atomic_load_n(&liveInfo->GetMarkFace().bitmap, std::memory_order_acquire);
        if (markBitmap == nullptr) {
            return false;
        }
        return markBitmap->IsMarked(offset);
    }

    template<Generation G>
inline bool RegionInfo::IsMarkedObject(MarkView<G> view, size_t offset)
    {
        CHECK(view.GetRegion() == this);
        if (!ValidateMarkView(view)) {
            return false;
        }
        if (view.GetEpoch() == GetMarkSnapshotEpoch<G>() && AllocatedAfterMarkStart(offset)) {
            return true;
        }
        LiveInfo* liveInfo = GetLiveInfoForView(view);
        if (liveInfo == nullptr) {
            return false;
        }
        if (!NoteMarkEpochOnRead(view, liveInfo)) {
            return false;
        }
        RegionBitmap* markBitmap =
            __atomic_load_n(&liveInfo->GetMarkFace().bitmap, std::memory_order_acquire);
        if (markBitmap == nullptr) {
            return false;
        }
        return markBitmap->IsMarked(offset);
    }

    template<Generation G>
inline bool RegionInfo::IsSurvivedObject(MarkView<G> view, size_t offset)
    {
        CHECK(view.GetRegion() == this);
        if (!ValidateMarkView(view)) {
            return false;
        }
        if (view.GetEpoch() == GetMarkSnapshotEpoch<G>() && AllocatedAfterMarkStart(offset)) {
            return true;
        }

        LiveInfo* liveInfo = GetLiveInfoForView(view);
        if (liveInfo == nullptr) {
            return false;
        }
        if (NoteMarkEpochOnRead(view, liveInfo)) {
            RegionBitmap* markBitmap =
                __atomic_load_n(&liveInfo->GetMarkFace().bitmap, std::memory_order_acquire);
            if (markBitmap != nullptr &&
                markBitmap->IsLive(offset)) {
                return true;
            }
        }
        if (G == Generation::Old) {
            RegionBitmap* resurrectBitmap =
                __atomic_load_n(&liveInfo->resurrectBitmap, std::memory_order_acquire);
            if (resurrectBitmap != nullptr &&
                resurrectBitmap->IsMarked(offset)) {
                return true;
            }
        }
        return false;
    }

inline bool RegionInfo::IsEnqueuedObject(size_t offset)
    {
        RegionBitmap* enqueBitmap = GetEnqueueBitmap();
        if (enqueBitmap == nullptr) {
            return false;
        }
        return enqueBitmap->IsMarked(offset);
    }

inline ALWAYS_INLINE size_t RegionInfo::GetAddressOffset(MAddress address)
    {
        DCHECK(GetRegionStart() <= address);
        return (address - GetRegionStart());
    }

inline void RegionInfo::RetirePage(RegionInfo* region, std::function<void()> retire)
    {
        {
            std::lock_guard<std::mutex> lock(pageRetirementMutex);
            const uintptr_t start = region->GetRegionStart();
            const size_t size = region->GetRegionSize();
            zoffset offset;
            CHECK(pageOwners.offset_for_address(start, &offset));
            CHECK(pageOwners.get(offset) == region);
            // zHeap.cpp:275-280 / zPageTable.cpp:59-65: remove the complete
            // old page while its descriptor still describes every granule.
            pageOwners.put(offset, size, nullptr);
            if (pageIterationCount != 0) {
                deferredPageRetirements.push_back(std::move(retire));
                return;
            }
        }
        retire();
    }

inline size_t RegionInfo::IndexedUnitCount(const std::vector<MemoryRange>& ranges)
    {
        CHECK(UNIT_SIZE != 0 && (UNIT_SIZE & (UNIT_SIZE - 1)) == 0);
        size_t count = 0;
        uintptr_t previousEnd = 0;
        for (const auto& range : ranges) {
            CHECK(!range.IsNull() && IsRepresentableLow48Range(range.start, range.size));
            CHECK(range.start >= previousEnd);
            CHECK(range.start % UNIT_SIZE == 0 && range.size % UNIT_SIZE == 0);
            CHECK(CheckedAddSize(count, range.size / UNIT_SIZE + 1, count));
            previousEnd = range.End();
        }
        CHECK(count != 0 && count - 1 < std::numeric_limits<uint32_t>::max());
        return count - 1;
    }

inline void RegionInfo::InitializeSegments(uintptr_t metadataEnd, const std::vector<MemoryRange>& ranges,
                                   MemMap* memoryOwner)
    {
        UnitInfo::totalUnitCount = IndexedUnitCount(ranges);
        UnitInfo::heapStartAddress = metadataEnd;
        UnitInfo::memoryOwner = memoryOwner;
        unitSegments.clear();
        size_t index = 0;
        for (const auto& range : ranges) {
            CHECK(IsRepresentableLow48Range(range.start, range.size));
            unitSegments.push_back(UnitSegment{ range, index });
            index += range.size / UNIT_SIZE + 1;
        }
        pageOwners.Reset();
        CHECK(pageOwners.Initialize(ranges.front().start, ranges.back().End() - ranges.front().start, UNIT_SIZE));
        // routedest: per-unit metadata is per-page metadata, so any growth here is a
        // D03b removes the two pointer-sized parallel route tables (16 bytes/unit).
        static_assert(sizeof(UnitInfo) == 216, "per-unit metadata size changed; it is per-page, so price it");
    }

inline size_t RegionInfo::FindUnitIndex(uintptr_t address)
    {
        auto next = std::upper_bound(unitSegments.begin(), unitSegments.end(), address,
            [](uintptr_t addr, const UnitSegment& segment) { return addr < segment.range.start; });
        if (next == unitSegments.begin()) {
            return UnitInfo::INVALID_IDX;
        }
        const auto& segment = *std::prev(next);
        return address < segment.range.End()
            ? segment.firstIndex + (address - segment.range.start) / UNIT_SIZE : UnitInfo::INVALID_IDX;
    }

inline bool RegionInfo::ContainsUnitRange(uintptr_t start, size_t size)
    {
        for (const auto& segment : unitSegments) {
            if (start >= segment.range.start && start < segment.range.End() && size <= segment.range.End() - start) {
                return true;
            }
        }
        return false;
    }

inline void RegionInfo::VisitPageOwners(const std::function<void(RegionInfo*)>& visitor)
    {
        // zPageTable.cpp:101-113: protect the whole callback lifetime,
        // including nested iteration and exceptional callback exits.
        PageIterationScope iteration;
        pageOwners.visit_unique(visitor);
    }

inline ALWAYS_INLINE RegionInfo* RegionInfo::TryGetRegionInfoAt(uintptr_t allocAddr)
    {
        zoffset offset;
        return pageOwners.offset_for_address(allocAddr, &offset) ? pageOwners.get(offset) : nullptr;
    }

inline RegionInfo* RegionInfo::GetRegionInfoAt(uintptr_t allocAddr)
    {
        RegionInfo* region = TryGetRegionInfoAt(allocAddr);
        CHECK_DETAIL(region != nullptr, "heap address %#zx has no owning region", allocAddr);
        return region;
    }

inline RegionInfo* RegionInfo::GetGhostFromRegionAt(uintptr_t allocAddr)
    {
        const size_t idx = FindUnitIndex(allocAddr);
        if (idx == UnitInfo::INVALID_IDX) {
            return nullptr;
        }
        UnitInfo* unit = UnitInfo::GetUnitInfo(idx);
        if (unit->GetMetadata().regionStateBitField.GetAtomicValue(
                RegionStateBitPos::IN_GHOST_FROM_REGION_FLAG, 1) == 0) {
            return nullptr;
        }
        RegionInfo* region = LoadUnitRole0(unit) == UnitRole::SUBORDINATE_UNIT
            ? unit->GetMetadata().ownerRegion0 : reinterpret_cast<RegionInfo*>(unit);
        if (region == nullptr ||
            __atomic_load_n(&unit->GetMetadata().ghostLifeId, __ATOMIC_ACQUIRE) !=
                region->GetRegionLifeId()) {
            return nullptr;
        }
#if defined(MRT_GC_UNIT_TESTS)
        RunGhostLookupTestHook(region);
#endif
        return region;
    }

inline void RegionInfo::InitFreeRegion(size_t unitIdx, size_t nUnit)
    {
        RegionInfo* region = reinterpret_cast<RegionInfo*>(RegionInfo::UnitInfo::GetUnitInfo(unitIdx));
        region->InitRegionInfo(nUnit, UnitRole::FREE_UNITS);
    }

inline RegionInfo* RegionInfo::InitRegion(size_t unitIdx, size_t nUnit, RegionInfo::UnitRole uclass)
    {
        RegionInfo* region = reinterpret_cast<RegionInfo*>(RegionInfo::UnitInfo::GetUnitInfo(unitIdx));
        region->InitRegion(nUnit, uclass);
        return region;
    }

inline RegionInfo* RegionInfo::InitRegionAt(uintptr_t addr, size_t nUnit, RegionInfo::UnitRole uclass)
    {
        size_t idx = RegionInfo::UnitInfo::GetUnitIdxAt(addr);
        return InitRegion(idx, nUnit, uclass);
    }

inline void RegionInfo::WaitCopiedBeforePayloadWipe(RegionInfo* region, const char* site)
    {
        if (region == nullptr) {
            return;
        }
        (void)site;
        ZForwardingLife::WaitPageDone(region->metadata.fwdOwner.load(std::memory_order_acquire));
    }

inline void RegionInfo::ClearUnits(size_t idx, size_t cnt,
                           FillerZeroDiag::Site site)
    {
        uintptr_t unitAddress = RegionInfo::GetUnitAddress(idx);
        size_t size = cnt * RegionInfo::UNIT_SIZE;
        CHECK(ContainsUnitRange(unitAddress, size));
        RegionInfo* wipeRegion = RegionInfo::TryGetRegionInfoAt(unitAddress);
        WaitCopiedBeforePayloadWipe(wipeRegion, "ClearUnits");

        DLOG(REGION, "clear dirty units[%zu+%zu, %zu) @[%#zx+%zu, %#zx)", idx, cnt, idx + cnt, unitAddress, size,
             unitAddress + size);
        // gcfwdfix: ring of zeroed ranges for WAS_LIVE_BEFORE_CLEAR (MRT_GCV2_TRACE_CLEAR=1).
        TraceClear::NoteRange(static_cast<MAddress>(unitAddress), size, "clear_units", nullptr, 0);

        FillerZeroDiag::Note(site, unitAddress, size);
        MapleRuntime::MemorySet(unitAddress, size, 0, size);
    }

inline size_t RegionInfo::CommitUnits(size_t idx, size_t cnt)
    {
        void* unitAddress = reinterpret_cast<void*>(RegionInfo::GetUnitAddress(idx));
        const size_t size = cnt * RegionInfo::UNIT_SIZE;
        // zPhysicalMemoryManager.cpp:230: retain and report the prefix.
        return UnitInfo::memoryOwner == nullptr ? 0 :
               UnitInfo::memoryOwner->CommitMemory(unitAddress, size);
    }

inline size_t RegionInfo::GetCommittedUnitBytes(size_t idx, size_t cnt)
    {
        return UnitInfo::memoryOwner == nullptr ? 0 :
               UnitInfo::memoryOwner->GetCommittedSize(GetUnitAddress(idx), cnt * UNIT_SIZE);
    }

inline void RegionInfo::ReleaseUnits(size_t idx, size_t cnt)
    {
        const size_t released = ReleaseUnitsPartial(idx, cnt);
        CHECK_DETAIL(released == cnt * RegionInfo::UNIT_SIZE,
                     "release outside heap reservation idx=%zu units=%zu released=%zu", idx, cnt, released);
    }

inline size_t RegionInfo::PublishUnitsRelease(size_t idx, size_t completed)
    {
        return UnitInfo::memoryOwner == nullptr ? 0 : UnitInfo::memoryOwner->PublishMemoryRelease(
            reinterpret_cast<void*>(GetUnitAddress(idx)), completed);
    }

inline size_t RegionInfo::ReleaseUnitsPartialImpl(size_t idx, size_t cnt, bool deferred)
    {
        void* unitAddress = reinterpret_cast<void*>(RegionInfo::GetUnitAddress(idx));
        size_t size = cnt * RegionInfo::UNIT_SIZE;
        CHECK(ContainsUnitRange(reinterpret_cast<uintptr_t>(unitAddress), size));
        RegionInfo* wipeRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<uintptr_t>(unitAddress));
        WaitCopiedBeforePayloadWipe(wipeRegion, "ReleaseUnits");

        DLOG(REGION, "release physical memory for units [%zu+%zu, %zu) @[%p+%zu, 0x%zx)", idx, cnt, idx + cnt,
             unitAddress, size, reinterpret_cast<uintptr_t>(unitAddress) + size);
        const size_t released = UnitInfo::memoryOwner == nullptr ? 0 :
                                (deferred ? UnitInfo::memoryOwner->ReleaseMemoryDeferred(unitAddress, size) :
                                            UnitInfo::memoryOwner->ReleaseMemory(unitAddress, size));
#ifdef CANGJIE_ASAN_SUPPORT
        if (released != 0) {
            Sanitizer::OnHeapMadvise(unitAddress, released);
        }
#endif
        return released;
    }

inline bool RegionInfo::IsEmpty() const
    {
        MRT_ASSERT(IsSmallRegion(), "wrong region type");
        return GetRegionAllocPtr() == GetRegionStart();
    }

inline size_t RegionInfo::GetRegionSize() const
    {
        MAddress regionStart = GetRegionStart();
        DCHECK(metadata.regionEnd > regionStart);
        return metadata.regionEnd - regionStart;
    }

inline size_t RegionInfo::GetRegionSizeForDetachCheck() const
    {
        const MAddress start = GetRegionStart();
        const MAddress end = metadata.regionEnd;
        return end > start && ContainsUnitRange(start, end - start) ? end - start : UNIT_SIZE;
    }

inline size_t RegionInfo::GetGhostRegionSize() const
    {
        // The old extent follows the forwarding incarnation. If no carrier is
        // installed (idle/test setup), the only valid extent is the page's
        // current own size.
        ZForwarding* carrier = GetFromPageCarrier();
        return carrier == nullptr ? GetRegionSize() : carrier->size();
    }

inline size_t RegionInfo::GetAvailableSize() const
    {
        MRT_ASSERT(IsSmallRegion(), "wrong region type");
        return GetRegionEnd() - GetRegionAllocPtr();
    }

inline RegionBitmap* RegionInfo::GetLiveStartBitmap()
    {
        const ZForwarding::FromPageView* from = GetFromPageView();
        if (from != nullptr) {
            return GetRouteMarkBitmap(from->liveInfo);
        }
        return GetOwnerMarkBitmap();
    }

inline MAddress RegionInfo::FindLiveObjectStart(MAddress field)
    {
        const MAddress start = GetRegionStart();
        if (field < start) {
            return 0;
        }
        if (IsLargeRegion()) {
            RegionBitmap* bitmap = GetLiveStartBitmap();
            if (bitmap != nullptr && bitmap->IsObjectStart(0)) {
                return start;
            }
            if (fromPageLargeMarked()) {
                return start;
            }
            return 0;
        }
        if (!IsSmallRegion()) {
            return 0;
        }
        RegionBitmap* bitmap = GetLiveStartBitmap();
        if (bitmap == nullptr) {
            return 0;
        }
        size_t off = field - start;
        off -= off % kMarkedBytesPerBit;
        for (;;) {
            if (bitmap->IsObjectStart(off)) {
                return start + off;
            }
            if (off < kMarkedBytesPerBit) {
                return 0;
            }
            off -= kMarkedBytesPerBit;
        }
    }

inline bool RegionInfo::fromPageLargeMarked()
    {
        const ZForwarding::FromPageView* from = GetFromPageView();
        return from != nullptr && from->largeMarked != 0;
    }

inline void RegionInfo::CollectLiveObjectStarts(std::vector<MAddress>& out)
    {
        out.clear();
        if (IsFreeRegion() || IsGarbageRegion() || IsOwnerKnownEmpty()) {
            return;
        }
        const MAddress start = GetRegionStart();
        if (IsLargeRegion()) {
            if (FindLiveObjectStart(start) == start) {
                out.push_back(start);
            }
            return;
        }
        if (!IsSmallRegion()) {
            return;
        }
        RegionBitmap* bitmap = GetLiveStartBitmap();
        if (bitmap == nullptr) {
            return;
        }
        const uintptr_t allocPtr = GetRegionAllocPtr();
        const size_t regionBytes = allocPtr > start ? (allocPtr - start) : 0;
        for (size_t off = 0; off < regionBytes; off += kMarkedBytesPerBit) {
            if (bitmap->IsObjectStart(off)) {
                out.push_back(start + off);
            }
        }
    }

inline void RegionInfo::InitFreeUnits()
    {

        size_t nUnit = GetUnitCount();
        UnitInfo* unit = reinterpret_cast<UnitInfo*>(this);
        UnitInfo::UnitInfoArray array = UnitInfo::UnitInfoArray(unit, nUnit);
        for (size_t i = 0; i < nUnit; ++i) {
            array[i].ToFreeRegion();
        }
    }





















inline bool RegionInfo::IsCompactRouteDestination(MAddress address) const
    {
        // The generation relocation set owns the sole from-to mapping.
        auto owner = ForwardingTable::RetainPageOwner(this);
        return IsCompacted() && owner && owner->find_from_by_to(address, nullptr);
    }





    template<Generation G>
inline void RegionInfo::PublishFromPageMetadata(MarkView<G> view)
    {
        CHECK(view.GetRegion() == this);
        const RegionLifeId life = view.GetLifeId();
        CHECK_DETAIL(ForwardingTable::PublishFromPageView(
                         this, GetLiveInfo(), view.GetEpoch(), GetRegionAllocPtr(), metadata.markStartAllocPtr,
                         static_cast<uint8_t>(G),
                         static_cast<uint8_t>((IsLargeRegion() ? IsCurrentFacePublished() : metadata.isMarked != 0) ||
                                              metadata.isResurrected != 0),
                         life),
                     "forwarding carrier missing at from-page publication region=%p", this);
    }

    template<Generation G>
inline __attribute__((always_inline)) void RegionInfo::PublishForwardingCarrier(MarkView<G> view)
    {
        SetUnitRole0(static_cast<UnitRole>(metadata.unitRole));
        PublishFromPageMetadata(view);
        // zForwarding.inline.hpp:67-70 — construction token = 1. Late retain
        // after detach (count 0) is refused; carrier and token are published
        // by this single product operation.
        ClearForwardingFaceReset();
        ClearCurrentMarkFace();
        metadata._generation_id = G == Generation::Young ? ZGenerationId::young : ZGenerationId::old;
        // Always install ghost membership, including a zero-live page. This is
        // what keeps the from-page carrier reachable until forwarding drain.
        SetInGhostRegion(1);
        metadata.nextRegionIdx0 = metadata.nextRegionIdx;

        size_t nUnit = GetUnitCount();
        UnitInfo* unit = reinterpret_cast<UnitInfo*>(this);
        UnitInfo::UnitInfoArray array = UnitInfo::UnitInfoArray(unit, nUnit);
        for (size_t i = 1; i < nUnit; i++) {
            UnitMetadata& mdata = array[i].GetMetadata();
            CHECK(static_cast<UnitRole>(mdata.unitRole) == UnitRole::SUBORDINATE_UNIT);
            CHECK(mdata.ownerRegion == this);
            CHECK(mdata.inGhostFromRegion == 0);
            array[i].SetUnitRole0(UnitRole::SUBORDINATE_UNIT);
            mdata.ownerRegion0 = this;
            array[i].SetInGhostRegion(1, GetRegionLifeId());
        }
    }

    template<Generation G>
inline void RegionInfo::PrepareForwardableRegion(MarkView<G> view)
    {
        CHECK(view.GetRegion() == this);
        CHECK(IsFromRegion());
        CHECK(static_cast<UnitRole>(metadata.unitRole) == UnitRole::SMALL_SIZED_UNITS);
        CHECK(metadata.inGhostFromRegion == 0);
        // marklate: freeze last-alloc phase before ghost snapshot (survives reuse).
        AllocPhaseDiag::FreezeRegion(GetRegionStart());
        (void)IsForwardingDone();
        // The preceding generation reset removed its forwarding set.
        ClearRelocationResiduals();
        // PORT_ZFORWARDING step 1: same event, recorded address-keyed as well.  Populated in
        // parallel with the region machinery so the two answers can be compared before either is
        // trusted; nothing reads it for decisions yet.
        CHECK_DETAIL(ForwardingTable::InstallPublicationBeforeCopy(GetRegionStart(), GetRegionSize(), this, G),
                     "forwarding table install failed before relocation region=%p range=[%#zx,%#zx)",
                     this, static_cast<size_t>(GetRegionStart()), static_cast<size_t>(GetRegionEnd()));
        // enrolphase: which side of the relocate-start flip does this enrolment land on?
        //
        // OpenJDK installs the relocation set once, in the concurrent select_relocation_set
        // (zGeneration.cpp:254 ZRelocationSet::install), and only then flips
        // (relocate_start -> flip_relocate_start, :918 -> :922).  Post-flip the set is closed, so
        // "painted with the current colour after the flip" implies "will not move this cycle".
        //
        // EvacuateYoungRegions calls PrepareForwardTable<Young> twice -- WCollector.cpp:6483 and
        // again at :6952 -- with the young flip between them.  An enrolment on the far side leaves
        // a window in which a region is still NORMAL while the current colour is already the new
        // one, and a value painted there is load-good but names an object that is about to move.
        // That matches the measured FORWARD population exactly (afterFlip=1, slotGood=1, hasTo=1,
        // 20/20), and it is why moving only the first flip (kFlipAfterFromSpace) changed nothing.
        //
        // The staleness predicate is not the hole: over ~2^20 non-NORMAL targets per run, across
        // six runs, zero escaped it.
        // gc_unit fixtures do not run Heap::Init, so CollectorProxy has no
        // current collector for this diagnostic-only phase sample.  Skip only
        // in the test configuration; product (macro off) always samples.
#if !defined(MRT_GC_UNIT_TESTS)
        NoteEnrolPhase();
#endif
        // sealcheck: snapshot is not yet sealed; geometry freeze is at RouteRegion ROUTING.
        SetMarkFaceSealed(false);
        // Shared boundary: publish immutable from-page metadata, forwarding
        // construction token, and ghost membership through one product edge.
        PublishForwardingCarrier(view);

    }

inline void RegionInfo::ClearGhostRegionBit()
    {
        if (IsGhostFromRegion()) {
            size_t nUnit = GetUnitCount();
            TraceClear::NoteRegionEvent(GetRegionStart(), nUnit * UNIT_SIZE, "clear_ghost", this,
                                        GetLiveByteCount(), 1, static_cast<unsigned int>(GetRegionType()),
                                        IsForwardingDone() ? 1u : 0u);
            UnitInfo* unit = reinterpret_cast<UnitInfo*>(this);
            UnitInfo::UnitInfoArray array = UnitInfo::UnitInfoArray(unit, nUnit);
            for (size_t i = 0; i < nUnit; i++) {
                array[i].SetInGhostRegion(0, GetRegionLifeId());
            }
        }
    }

inline void RegionInfo::ClearGhostFromRegionBits()
    {
        const size_t nUnit = GetGhostRegionUnitCount();
        UnitInfo* unit = reinterpret_cast<UnitInfo*>(this);
        UnitInfo::UnitInfoArray array = UnitInfo::UnitInfoArray(unit, nUnit);
        for (size_t i = 0; i < nUnit; i++) {
            array[i].SetInGhostRegion(0, GetRegionLifeId());
        }
    }

inline void RegionInfo::DispelGhostFromRegion()
    {
        // fwdinflight: this is one of the three edges that retire from-side route state, and
        // it is unconditional -- nothing here waits for a reader. ZGC's equivalent,
        // ZForwarding::detach_page (zForwarding.cpp:171-181), blocks until _ref_count is zero.
        // Count what we would be invalidating. Default off; never blocks.

        // portmutreloc: hold the forwarding drain across the whole body. It is held
        // run while a retained reader is inside the route lookup or a mutator copy.
        InPlaceClaimScope drain(this, ZForwardingLife::Retire::DISPEL_GHOST);
        // PORT_ZFORWARDING step 1: the retirement edge.  ZGC's equivalent is refcount-driven
        // (ZForwarding::detach_page waits for _ref_count == 0); recording the removal here first
        // lets step 3 change *when* it happens without changing *where*.
        const size_t nUnit = GetGhostRegionUnitCount();
        ClearGhostFromRegionBits();
        LiveInfoArena::GetLiveInfoArena().RecycleOwnerBitmaps(GetLiveInfo());
        dispelGhostCount.fetch_add(1, std::memory_order_relaxed);
        TraceClear::NoteRegionEvent(GetRegionStart(), nUnit * UNIT_SIZE, "dispel", this, GetLiveByteCount(),
                                    static_cast<unsigned int>(IsGhostFromRegion()),
                                    static_cast<unsigned int>(GetRegionType()),
                                    IsForwardingDone() ? 1u : 0u);
        // fysfixb: name who clears the ghost bit (PrepareFromRegionList peer path).
        VLOG(REPORT,
             "[GCV2][ghost-dispel] region=%p start=%#zx nUnit=%zu live=%zu route=%u young=%u",
             this, GetRegionStart(), nUnit, GetLiveByteCount(),
              IsForwardingDone() ? 1u : 0u,
             static_cast<unsigned>(IsYoungRegion()));
        // Publish route retirement before detaching the table. A reader that observes
        // the atomic nullptr then also observes NORMAL and soft-misses in GetRoute.
        SetMarkFaceSealed(false);
        // The old top/livemap disappeared with the forwarding carrier above;
        // only page-owned ghost/route state is reset in this body.
    }

inline bool RegionInfo::IsGhostFromRegion() const
    {
        const bool ghost = metadata.regionStateBitField.GetAtomicValue(
            RegionStateBitPos::IN_GHOST_FROM_REGION_FLAG, 1) != 0;
        if (!ghost) {
            return false;
        }
        return __atomic_load_n(&metadata.ghostLifeId, __ATOMIC_ACQUIRE) == GetRegionLifeId();
    }

inline void RegionInfo::AssertGhostClearedAfterReuse(size_t nUnit) const
    {
        CHECK(!IsGhostFromRegion());
        size_t baseIdx = GetUnitIdx();
        for (size_t i = 1; i < nUnit; i++) {
            MAddress addr = GetUnitAddress(baseIdx + i);
            CHECK(!InGhostFromRegion(from_region_addr(addr)));
        }
    }

    template<Generation G>
inline void RegionInfo::ClearLiveInfo(MarkView<G> view)
    {
        CHECK(view.GetRegion() == this);
        UnitRole unitRole = LoadUnitRole(reinterpret_cast<UnitInfo*>(this));
        if (unitRole == UnitRole::FREE_UNITS) {
            return;
        }
        CHECK_DETAIL(unitRole == UnitRole::SMALL_SIZED_UNITS || unitRole == UnitRole::LARGE_SIZED_UNITS,
                     "ClearLiveInfo must be called on a region head");
        SurvNodeDiag::NoteClear(this, SurvNodeDiag::CLEAR_LIVE_INFO, true);
        // ZGC mark-start allocation watermark (zPage is_allocating). Capture
        // before the epoch bump so VisitLive / IsKnownEmpty / IsMarkedObject
        // see objects bumped after this point as implicitly live.
        metadata.markStartAllocPtr = GetRegionAllocPtr();
        // GenerationCycle::Begin already advanced the owning generation's seqnum.
        // Ordinary livemap metadata and segments reset lazily on the first mark,
        // including the single-object large-page map.
        if (G == Generation::Old) {
            NoteRetainedClear(RETAINED_OP_CLEAR_ALL);
            // A new major mark supersedes the retained major snapshot.  Young
            // clears deliberately leave this old/major authority intact.
            FreeRetainedMarkWords();
            metadata.retainedLiveInfo = nullptr;
            metadata.retainedLiveInfoEpoch = 0;
            metadata.retainedLiveInfoCoveredUpTo = 0;
            metadata.retainedLifeId = 0;
            // ClearLiveInfo<Old> starts a new retained-snapshot cycle.  It is
            // the only same-region-life boundary allowed to disarm the bit.
            metadata.retainedEverPreserved = 0;
        }
        SetMarkFaceSealed(false);
    }

inline bool RegionInfo::RetainForwarding()
    {
        auto owner = ForwardingTable::RetainPageOwner(this);
        return owner && owner->retain_page();
    }

inline void RegionInfo::ReleaseForwarding()
    {
        auto owner = ForwardingTable::RetainPageOwner(this);
        CHECK(owner);
        owner->release_page();
    }

inline bool RegionInfo::ClaimForwarding()
    {
        auto owner = ForwardingTable::RetainPageOwner(this);
        return owner && owner->claim();
    }

inline void RegionInfo::MarkForwardingDone()
    {
        auto owner = ForwardingTable::RetainPageOwner(this);
        if (owner && ZForwardingLife::CurrentPageWork() != owner.get()) owner->mark_done();
    }

inline bool RegionInfo::IsForwardingDone() const
    {
        auto owner = ForwardingTable::RetainPageOwner(this);
        return owner && owner->is_done();
    }

inline bool RegionInfo::IsForwardingFaceCurrent() const
    {
        // Prefer the current LiveInfo face, just as ZGC's page seqnum check
        // does.  A fresh face means forwarding-done is stale even if the
        // immutable from-page carrier has already retired.  Otherwise the
        // carrier epoch distinguishes a real copy (same face) from an older
        // forwarding completion; absent both identities, keep the historical
        // conservative guard.
        const uint64_t snapshotEpoch = GetSnapshotEpoch();
        LiveInfo* liveInfo = GetLiveInfo();
        if (liveInfo != nullptr &&
            liveInfo->GetMarkFace().epoch.load(std::memory_order_acquire) == snapshotEpoch) {
            return false;
        }
        if (HasFromPageMetadata()) {
            const ZForwarding::FromPageView* from = GetFromPageView();
            return from != nullptr && from->epoch == snapshotEpoch;
        }
        if (IsCurrentFacePublished()) {
            return false;
        }
        return true;
    }

inline bool RegionInfo::IsForwardingFaceReset() const
    {
        return (__atomic_load_n(&metadata.retainedPreserveCnt, __ATOMIC_ACQUIRE) &
            FORWARDING_FACE_RESET_BIT) != 0;
    }

inline void RegionInfo::ClearCurrentMarkFace()
    {
        LiveInfo* live = GetLiveInfo();
        if (live != nullptr) {
            live->GetMarkFace().epoch.store(0, std::memory_order_relaxed);
        }
    }

inline bool RegionInfo::IsCurrentFacePublished() const
    {
        LiveInfo* live = GetLiveInfo();
        const uint64_t seqnum = GetSnapshotEpoch();
        return seqnum != 0 && live != nullptr &&
            live->GetMarkFace().epoch.load(std::memory_order_acquire) == seqnum;
    }

inline int32_t RegionInfo::ForwardingRefCount() const
    {
        auto owner = ForwardingTable::RetainPageOwner(this);
        return owner ? owner->ref_count().load(std::memory_order_acquire) : 0;
    }

inline bool RegionInfo::ForwardingClaimed() const
    {
        auto owner = ForwardingTable::RetainPageOwner(this);
        return owner && owner->claimed().load(std::memory_order_acquire);
    }

inline void RegionInfo::SetRegionType(RegionType type)
    {
        metadata.regionStateBitField.SetAtomicValue(RegionStateBitPos::REGION_TYPE_FLAG, BIT_LENGTH,
                                                    static_cast<uint8_t>(type));
    }

inline void RegionInfo::SetTraceRegionFlag(uint8_t flag)
    {
        uint8_t prev = metadata.isTraceRegion;
        metadata.regionStateBitField.SetAtomicValue(RegionStateBitPos::TRACE_REGION_FLAG, 1, flag);
        // blackmark: track 1→0 clears (EnlistFullThreadLocalRegion / HandleTraceRegions).
        if (AllocPhaseDiag::Enabled()) {
            if (flag == 0 && prev != 0) {
                AllocPhaseDiag::NoteTraceFlagCleared(GetRegionStart());
            } else if (flag != 0) {
                AllocPhaseDiag::NoteTraceFlagSet(GetRegionStart());
            }
        }
    }



inline void RegionInfo::SetInGhostRegion(uint8_t flag)
    {
        const RegionLifeId life = GetRegionLifeId();
        __atomic_store_n(&metadata.ghostLifeId, life, __ATOMIC_RELEASE);
        metadata.regionStateBitField.SetAtomicValue(RegionStateBitPos::IN_GHOST_FROM_REGION_FLAG, 1, flag);
        if (flag != 0) {
        }
    }

inline MarkView<Generation::Old> RegionInfo::PromoteYoungRegion(MarkView<Generation::Young> youngView)
    {
        CHECK_DETAIL(youngView.GetRegion() == this, "young promotion view belongs to another region");
        CHECK_DETAIL(IsYoungRegion(), "cannot promote an old region %p", this);
        CHECK_DETAIL(youngView.GetEpoch() == GetMarkSnapshotEpoch<Generation::Young>(),
                     "cannot promote region %p through a stale young mark view", this);
        SetOldMarkedRegionFlag(0);
        SetEnqueuedRegionFlag(0);
        SetResurrectedRegionFlag(0);

        metadata.markStartAllocPtr = 0;
        __atomic_store_n(&metadata.liveInfo, static_cast<LiveInfo*>(nullptr), std::memory_order_release);
        SetYoungRegionFlag(0);
        SetYoungAge(0);
        InitializeLiveInfo();
        return GetMarkView<Generation::Old>();
    }

inline void RegionInfo::SetYoungAge(uint8_t age)
    {
        CHECK(age <= MAX_YOUNG_AGE);
        metadata.regionStateBitField.SetAtomicValue(RegionStateBitPos::YOUNG_AGE_FLAG, YOUNG_AGE_BIT_LENGTH, age);
    }

inline uint8_t RegionInfo::GetYoungAge() const
    {
        return static_cast<uint8_t>(metadata.regionStateBitField.GetAtomicValue(
                                        RegionStateBitPos::YOUNG_AGE_FLAG, YOUNG_AGE_BIT_LENGTH) >>
                                    RegionStateBitPos::YOUNG_AGE_FLAG);
    }

inline RegionInfo::RegionType RegionInfo::GetRegionType() const
    {
        return static_cast<RegionType>(
            metadata.regionStateBitField.GetAtomicValue(RegionStateBitPos::REGION_TYPE_FLAG, BIT_LENGTH));
    }

inline bool RegionInfo::AllocatedAfterMarkStart(size_t offset) const
    {
        uintptr_t water = metadata.markStartAllocPtr;
        if (water == 0) {
            return false;
        }
        MAddress start = GetRegionStart();
        if (water <= start) {
            return true;
        }
        return offset >= static_cast<size_t>(water - start);
    }

inline bool RegionInfo::HasMarkStartAllocGap() const
    {
        uintptr_t water = metadata.markStartAllocPtr;
        if (water == 0) {
            return false;
        }
        return GetRegionAllocPtr() > water;
    }

inline int32_t RegionInfo::IncRawPointerObjectCount()
    {
        int32_t oldCount = __atomic_fetch_add(&metadata.rawPointerObjectCount, 1, __ATOMIC_SEQ_CST);
        CHECK_DETAIL(oldCount >= 0, "region %p has wrong raw pointer count %d", this);
        CHECK_DETAIL(oldCount < MAX_RAW_POINTER_COUNT, "inc raw-pointer-count overflow");
        return oldCount;
    }

inline int32_t RegionInfo::DecRawPointerObjectCount()
    {
        int32_t oldCount = __atomic_fetch_sub(&metadata.rawPointerObjectCount, 1, __ATOMIC_SEQ_CST);
        CHECK_DETAIL(oldCount > 0, "dec raw-pointer-count underflow, please check whether releaseRawData is overused.");
        return oldCount;
    }

inline bool RegionInfo::CompareAndSwapRawPointerObjectCount(int32_t expectVal, int32_t newVal)
    {
        return __atomic_compare_exchange_n(&metadata.rawPointerObjectCount, &expectVal, newVal, false, __ATOMIC_SEQ_CST,
                                           __ATOMIC_ACQUIRE);
    }

inline uintptr_t RegionInfo::Alloc(size_t size)
    {
        size_t limit = GetRegionEnd();
        if (metadata.allocPtr + size <= limit) {
            uintptr_t addr = metadata.allocPtr;
            metadata.allocPtr += size;
            return addr;
        } else {
            return 0;
        }
    }

inline uintptr_t RegionInfo::AtomicAlloc(size_t size)
    {
        // zPage.inline.hpp:451-479: reject an out-of-page top before CAS.
        // Failed allocations must never move the published allocation frontier.
        uintptr_t addr = __atomic_load_n(&metadata.allocPtr, __ATOMIC_ACQUIRE);
        const uintptr_t limit = GetRegionEnd();
        for (;;) {
            if (addr > limit || size > limit - addr) {
                return 0;
            }
            const uintptr_t next = addr + size;
            if (__atomic_compare_exchange_n(&metadata.allocPtr, &addr, next, false,
                                             __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                return addr;
            }
        }
    }

inline bool RegionInfo::UndoAllocObjectAtomic(uintptr_t addr, size_t size)
    {
        uintptr_t expected = addr + size;
        return __atomic_compare_exchange_n(&metadata.allocPtr, &expected, addr, false, __ATOMIC_ACQ_REL,
                                           __ATOMIC_ACQUIRE);
    }

inline bool RegionInfo::IsPinnedRegion() const
    {
        return (static_cast<RegionType>(metadata.regionType) == RegionType::FULL_PINNED_REGION) ||
            (static_cast<RegionType>(metadata.regionType) == RegionType::RECENT_PINNED_REGION);
    }

inline RegionInfo* RegionInfo::GetPrevRegion() const
    {
        if (UNLIKELY(metadata.prevRegionIdx == NULLPTR_IDX)) {
            return nullptr;
        }
        return reinterpret_cast<RegionInfo*>(UnitInfo::GetUnitInfo(metadata.prevRegionIdx));
    }

inline void RegionInfo::SetPrevRegion(const RegionInfo* r)
    {
        if (UNLIKELY(r == nullptr)) {
            metadata.prevRegionIdx = NULLPTR_IDX;
            return;
        }
        size_t prevIdx = r->GetUnitIdx();
        MRT_ASSERT(prevIdx < NULLPTR_IDX, "exceeds the maximum limit for region info");
        metadata.prevRegionIdx = static_cast<uint32_t>(prevIdx);
    }

inline RegionInfo* RegionInfo::GetNextRegion() const
    {
        if (UNLIKELY(metadata.nextRegionIdx == NULLPTR_IDX)) {
            return nullptr;
        }
        DCHECK(metadata.nextRegionIdx < UnitInfo::totalUnitCount);
        return reinterpret_cast<RegionInfo*>(UnitInfo::GetUnitInfo(metadata.nextRegionIdx));
    }

inline RegionInfo* RegionInfo::GetNextGhostRegion() const
    {
        if (UNLIKELY(metadata.nextRegionIdx0 == NULLPTR_IDX)) {
            return nullptr;
        }
        DCHECK(metadata.nextRegionIdx0 < UnitInfo::totalUnitCount);
        return reinterpret_cast<RegionInfo*>(UnitInfo::GetUnitInfo(metadata.nextRegionIdx0));
    }

inline void RegionInfo::SetNextRegion(const RegionInfo* r)
    {
        if (UNLIKELY(r == nullptr)) {
            metadata.nextRegionIdx = NULLPTR_IDX;
            return;
        }
        size_t nextIdx = r->GetUnitIdx();
        MRT_ASSERT(nextIdx < NULLPTR_IDX, "exceeds the maximum limit for region info");
        metadata.nextRegionIdx = static_cast<uint32_t>(nextIdx);
    }

inline bool RegionInfo::IsUnmovableFromRegion() const
    {
        RegionType type = GetRegionType();
        return type == RegionType::UNMOVABLE_FROM_REGION || type == RegionType::RAW_POINTER_PINNED_REGION;
    }

inline bool RegionInfo::IsValidRegion() const
    {
        return static_cast<UnitRole>(metadata.unitRole) == UnitRole::SMALL_SIZED_UNITS ||
            static_cast<UnitRole>(metadata.unitRole) == UnitRole::LARGE_SIZED_UNITS;
    }

inline RegionBitmap* RegionInfo::GetCurrentLiveMap() const
    {
        LiveInfo* live = GetLiveInfo();
        const uint64_t seqnum = GetSnapshotEpoch();
        if (live == nullptr || seqnum == 0 ||
            live->GetMarkFace().epoch.load(std::memory_order_acquire) != seqnum) {
            return nullptr;
        }
        return __atomic_load_n(&live->GetMarkFace().bitmap, std::memory_order_relaxed);
    }

inline uint64_t RegionInfo::GetLiveByteCount() const
    {
        RegionBitmap* bitmap = GetCurrentLiveMap();
        return bitmap == nullptr ? 0 : bitmap->GetLiveBytes();
    }

inline uint32_t RegionInfo::GetLiveObjectCount() const
    {
        RegionBitmap* bitmap = GetCurrentLiveMap();
        return bitmap == nullptr ? 0 : static_cast<uint32_t>(bitmap->GetLiveObjects());
    }

inline void RegionInfo::AddLiveCounts(uint32_t objects, uint64_t bytes)
    {
        RegionBitmap* bitmap = GetCurrentLiveMap();
        CHECK(bitmap != nullptr);
        bitmap->AddLiveCounts(objects, bytes);
    }

inline bool RegionInfo::IsKnownEmpty(MarkView<Generation::Old> view) const
    {
        CHECK(view.GetRegion() == this);
        if (!ValidateMarkView(view)) {
            return false;
        }
        // ZGC allocate-black: a mark-start watermark gap means objects were
        // born after ClearLiveInfo and are implicitly live. Do not treat the
        // region empty, and do not count them as explicitly marked.
        if (HasMarkStartAllocGap()) {
            return false;
        }
        const uint64_t liveBytes = GetLiveByteCount();
        const bool auth = IsLiveCountAuthoritative();
        bool markedThisCycle = false;
        bool keepNullFace = false;
        bool keepEpoch = false;
        if (IsLargeRegion()) {
            if (view.GetEpoch() != GetMarkSnapshotEpoch<Generation::Old>()) {
                keepEpoch = true;
            } else {
                markedThisCycle = true;
            }
        } else {
            LiveInfo* liveInfo = __atomic_load_n(&metadata.liveInfo, std::memory_order_acquire);
            if (liveInfo == nullptr) {
                markedThisCycle = false;
                keepNullFace = true;
            } else if (liveInfo->GetMarkFace().epoch.load(std::memory_order_acquire) !=
                           view.GetEpoch() ||
                       view.GetEpoch() != GetMarkSnapshotEpoch<Generation::Old>()) {
                markedThisCycle = false;
                keepEpoch = true;
            } else {
                markedThisCycle = true;
            }
        }
        const bool emptyByMark = markedThisCycle && (IsLargeRegion()
            ? GetMarkedRegionFlag(view) == 0
            : (liveBytes == 0));

        if (!ikeAtexitInstalled.exchange(true, std::memory_order_relaxed)) {
            std::atexit([]() {
                std::fprintf(stderr,
                             "[GCV2][ike-keep] atexit trueEmpty=%zu keep=%zu keepBytes=%zu "
                             "nullFace=%zu epoch=%zu\n",
                             ikeTrueEmpty.load(std::memory_order_relaxed),
                             ikeConservativeKeep.load(std::memory_order_relaxed),
                             ikeConservativeKeepBytes.load(std::memory_order_relaxed),
                             ikeNullFaceKeep.load(std::memory_order_relaxed),
                             ikeEpochKeep.load(std::memory_order_relaxed));
                std::fflush(stderr);
            });
        }
        if (!auth) {
            return false;
        }
        if (emptyByMark) {
            ikeTrueEmpty.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        if (keepNullFace || keepEpoch) {
            size_t n = ikeConservativeKeep.fetch_add(1, std::memory_order_relaxed) + 1;
            ikeConservativeKeepBytes.fetch_add(GetRegionSize(), std::memory_order_relaxed);
            if (keepNullFace) {
                ikeNullFaceKeep.fetch_add(1, std::memory_order_relaxed);
            }
            if (keepEpoch) {
                ikeEpochKeep.fetch_add(1, std::memory_order_relaxed);
            }
            if (n <= 8 || (n & (n - 1)) == 0) {
                LOG(RTLOG_ERROR,
                    "[GCV2][ike-keep] n=%zu region=%p start=%#zx nullFace=%u epoch=%u "
                    "live=%llu — not empty (unmarked this cycle)",
                    n, this, GetRegionStart(), static_cast<unsigned>(keepNullFace),
                    static_cast<unsigned>(keepEpoch),
                    static_cast<unsigned long long>(liveBytes));
            }
        }
        return false;
    }

inline bool RegionInfo::IsKnownYoungEmpty(MarkView<Generation::Young> view) const
    {
        CHECK(view.GetRegion() == this);
        if (!ValidateMarkView(view)) {
            return false;
        }
        if (HasMarkStartAllocGap()) {
            return false;
        }
        const uint64_t liveBytes = GetLiveByteCount();
        const bool auth = IsLiveCountAuthoritative();
        bool markedThisCycle = false;
        if (IsLargeRegion()) {
            markedThisCycle = view.GetEpoch() == GetMarkSnapshotEpoch<Generation::Young>();
        } else {
            LiveInfo* liveInfo = __atomic_load_n(&metadata.liveInfo, std::memory_order_acquire);
            if (liveInfo == nullptr) {
                markedThisCycle = false;
            } else {
                markedThisCycle = liveInfo->GetMarkFace().epoch.load(std::memory_order_acquire) ==
                        view.GetEpoch() &&
                    view.GetEpoch() == GetMarkSnapshotEpoch<Generation::Young>();
            }
        }
        const bool emptyByMark = markedThisCycle && (IsLargeRegion()
            ? GetMarkedRegionFlag(view) == 0
            : (liveBytes == 0));
        return auth && emptyByMark;
    }

inline bool RegionInfo::IsSafeKnownEmpty(MarkView<Generation::Old> view)
    {
        if (!IsKnownEmpty(view)) {
            return false;
        }
        if (GetRegionAllocPtr() <= GetRegionStart()) {
            return true;
        }
        // Examined: either large, or we had a mark face this cycle that is now stale/null
        // (authority already required by IsKnownEmpty). Residual bitmap pointer may remain.
        return GetMarkBitmap(view) != nullptr || GetResurrectBitmap() != nullptr || IsLargeRegion() ||
            __atomic_load_n(&metadata.liveInfo, std::memory_order_acquire) != nullptr;
    }

inline bool RegionInfo::IsSafeKnownYoungEmpty(MarkView<Generation::Young> view)
    {
        if (!IsKnownYoungEmpty(view)) {
            return false;
        }
        if (GetRegionAllocPtr() <= GetRegionStart()) {
            return true;
        }
        return GetMarkBitmap(view) != nullptr || IsLargeRegion() ||
            __atomic_load_n(&metadata.liveInfo, std::memory_order_acquire) != nullptr;
    }

    template<Generation G>
inline void RegionInfo::ResetLiveMapAfterForward(MarkView<G> view)
    {
        CHECK(view.GetRegion() == this);
        // Forwarding is the last reader of this mark face. Copy it while the
        // supplied view is still current; a partially forwarded page may stay
        // UNMOVABLE_FROM after the epoch bump and still contain live holders.
        if (!IsLargeRegion()) {
            PreserveRetainedLiveInfo();
        }
        SetForwardingFaceReset();
        ClearCurrentMarkFace();

        if (IsLargeRegion()) {
            SetMarkedRegionFlag(view, 0);
        }
    }

    template<Generation G>
inline void RegionInfo::VerifyLiveBooks(MarkView<G> view, const char* where)
    {
        CHECK(view.GetRegion() == this);
        liveCrossCheckCount.fetch_add(1, std::memory_order_relaxed);
        if (!IsLiveCountAuthoritative()) {
            return;
        }
        const uint64_t liveBytes = GetLiveByteCount();
        const bool emptyByMark = G == Generation::Young
            ? IsKnownYoungEmpty(MarkView<Generation::Young>(this, view.GetEpoch(), view.GetLifeId()))
            : IsKnownEmpty(MarkView<Generation::Old>(this, view.GetEpoch(), view.GetLifeId()));
        // Homology only when this cycle marked the page. Unmarked ∧ live==0 is
        // conservative-keep (cjpmnull2), not a book error (zPage.inline.hpp:223-225).
        const bool emptyByLive = (liveBytes == 0);
        if (emptyByMark == emptyByLive || (!emptyByMark && emptyByLive)) {
            return;
        }
        size_t n = liveCrossMismatchCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (!liveCrossAtexitInstalled.exchange(true, std::memory_order_relaxed)) {
            std::atexit([]() {
                std::fprintf(stderr, "[GCV2][livesame][crosscheck] atexit checks=%zu mismatch=%zu\n",
                             liveCrossCheckCount.load(std::memory_order_relaxed),
                             liveCrossMismatchCount.load(std::memory_order_relaxed));
                std::fflush(stderr);
            });
        }
        if (n <= 32) {
            LOG(RTLOG_ERROR,
                "[GCV2][livesame][crosscheck] where=%s region=%p liveBytes=%llu emptyByMark=%u "
                "emptyByLive=%u n=%zu",
                where != nullptr ? where : "?", this, static_cast<unsigned long long>(liveBytes),
                static_cast<unsigned>(emptyByMark), static_cast<unsigned>(emptyByLive), n);
        }
    }

inline void RegionInfo::RemoveFromList()
    {
        RegionInfo* prev = GetPrevRegion();
        RegionInfo* next = GetNextRegion();
        if (prev != nullptr) {
            prev->SetNextRegion(next);
        }
        if (next != nullptr) {
            next->SetPrevRegion(prev);
        }
        this->SetNextRegion(nullptr);
        this->SetPrevRegion(nullptr);
    }











inline ALWAYS_INLINE void RegionInfo::CheckObjectSize(
        const BaseObject* obj, size_t objSize, MAddress regionStart, MAddress regionEnd) const
    {
        // Always-on TypeInfo range check: same predicate as CheckTypeInfoRegion rule 3
        // (VerifyHeap.cpp:105-108) — tip ∈ heap address range is a defect.
        // Default: count + one-shot dump (no abort). Fatal: MRT_GCV2_TIPINHEAP_FATAL=1.
        TypeInfo* tip = obj->GetTypeInfo();
        if (UNLIKELY(Heap::IsHeapAddress(tip))) {
            ReportTypeInfoInHeap(obj, tip, objSize, regionStart, regionEnd);
        }
        MAddress objAddr = reinterpret_cast<MAddress>(obj);
        // kMarkedBytesPerBit is 8, matching Allocator::ALLOC_ALIGN (Allocator.h:19).
        if (UNLIKELY(objSize == 0 || (objSize % kMarkedBytesPerBit) != 0 || objSize > regionEnd - objAddr)) {
            ReportInvalidObjectSize(obj, objSize, regionStart, regionEnd);
        }
    }




inline RegionInfo::UnitRole RegionInfo::LoadUnitRole0(UnitInfo* unit)
    {
        return static_cast<UnitRole>(
            unit->GetMetadata().unitRoleBitField.GetAtomicValue(BIT_LENGTH, BIT_LENGTH) >> BIT_LENGTH);
    }

inline void RegionInfo::BumpRegionLifeId()
    {
        RegionLifeId old = metadata.regionLifeId.load(std::memory_order_relaxed);
        for (;;) {
            if (UNLIKELY(old == std::numeric_limits<RegionLifeId>::max())) {
                LOG(RTLOG_FATAL,
                    "[LIFECLOCK][REGION_LIFE_ID_OVERFLOW] region=%p life=%llu; wraparound is forbidden",
                    this, static_cast<unsigned long long>(old));
                return;
            }
            if (metadata.regionLifeId.compare_exchange_weak(old, old + 1, std::memory_order_release,
                                                            std::memory_order_relaxed)) {
                // A new region life is the hard boundary for the monotonic
                // retained Preserve history.
                metadata.retainedEverPreserved = 0;
                return;
            }
        }
    }

inline void RegionInfo::InitRegionInfo(size_t nUnit, UnitRole uClass)
    {
        CHECK(ContainsUnitRange(GetRegionStart(), nUnit * UNIT_SIZE));
        CHECK(TryGetRegionInfoAt(GetRegionStart()) == nullptr);
        CHECK_DETAIL(GetRegionListOwner() == nullptr, "reinitializing a region still owned by a list");

        M0Correlation::InvalidateRegionBindings(GetRegionStart(), GetRegionLifeId());
        SetUnitRole(UnitRole::FREE_UNITS);
        // Invalidate every old-life carrier before clearing any of its payload.
        // Readers either retain the old page (detachgate) or observe this bump and
        // reject the old incarnation; there is no wraparound fallback.
        BumpRegionLifeId();
        {
            uint8_t cur = __atomic_load_n(&metadata.regionLifeSequence, __ATOMIC_RELAXED);
            uint8_t next = static_cast<uint8_t>((cur + 1) & 0x7f);
            __atomic_store_n(&metadata.regionLifeSequence, next, __ATOMIC_RELEASE);
        }
        // See DispelGhostFromRegion: retire the route before detaching its compact table.
        ForwardingTable::ClearPageOwner(this);
        WaitCopiedBeforePayloadWipe(this, "InitRegionInfo");
        LiveInfoArena::GetLiveInfoArena().RecyclePageLiveInfo(this);
        SetYoungRegionFlag(0);
        metadata.allocPtr = GetRegionStart();
        metadata.regionEnd = metadata.allocPtr + nUnit * RegionInfo::UNIT_SIZE;
        // Unset until ClearLiveInfo starts a mark. 0 so idle / test regions do
        // not treat every object as allocate-black.
        metadata.markStartAllocPtr = 0;
        metadata.prevRegionIdx = NULLPTR_IDX;
        metadata.nextRegionIdx = NULLPTR_IDX;
        // Ghost walk (PrepareFromRegionList) follows nextRegionIdx0. A reused
        // region that still named its previous-life successor kept a retired
        // from-space chain alive across InitRegion (RegionManager.h:782).
        metadata.nextRegionIdx0 = NULLPTR_IDX;
        metadata.regionListOwner.store(nullptr, std::memory_order_relaxed);
        metadata.censusBoundaryOffset = 0;
        metadata.liveInfo = nullptr;
        ClearCurrentMarkFace();
        FreeRetainedMarkWords();
        metadata.retainedLiveInfo = nullptr;
        metadata.retainedLiveInfoEpoch = 0;
        metadata.retainedLiveInfoCoveredUpTo = 0;
        metadata.retainedLifeId = 0;
        // holderlive (F2): new region life — its predecessor's snapshot history does not
        // describe the objects that are about to be allocated here.
        metadata.retainedPreserveCnt = 0;
        metadata.retainedClearCnt = 0;
        metadata.retainedLastOp = RETAINED_OP_NONE;
        // routedest: this is the reuse edge named in the defect. TakeRegion has already run
        // ClearUnits over this payload; if a published route still names this region, the
        // route now answers into zeroed (or freshly re-allocated) memory. Count it here
        // rather than at ClearUnits because this is the one call that runs exactly once per
        // reuse. The hold is deliberately NOT cleared: reaching this point while held means
        // a reclaim gate was bypassed, and leaving the flag set keeps the region out of the
        // next collection set instead of silently papering over the escape.
        SetRegionType(RegionType::FREE_REGION);
        SetTraceRegionFlag(0);
        SetNotRelocatableThisCycle(0);
        // Ghost lives in unit metadata, not payload: ClearUnits cannot clear it.
        // TakeRegion reuses garbage without DispelGhostFromRegion.
        SetInGhostRegion(0);
        SetOldMarkedRegionFlag(0);
        SetEnqueuedRegionFlag(0);
        SetResurrectedRegionFlag(0);
        SetMarkFaceSealed(false);
        __atomic_store_n(&metadata.rawPointerObjectCount, 0, __ATOMIC_SEQ_CST);
        if (uClass != UnitRole::FREE_UNITS) {
            InitializeLiveInfo();
        }
        SetUnitRole(uClass);
    }

inline void RegionInfo::InitRegion(size_t nUnit, UnitRole uClass)
    {
        InitRegionInfo(nUnit, uClass);



        // initialize region's subordinate units.

        UnitInfo* unit = reinterpret_cast<UnitInfo*>(this);
        UnitInfo::UnitInfoArray array = UnitInfo::UnitInfoArray(unit, nUnit);
        for (size_t i = 1; i < nUnit; i++) {
            array[i].InitSubordinateUnit(this);
        }
        AssertGhostClearedAfterReuse(nUnit);
        // zHeap.cpp:250-254 / zPageTable.cpp:44-54: publish only after the
        // entire descriptor (including its subordinate ABI units) is ready.
        zoffset offset;
        CHECK(pageOwners.offset_for_address(GetRegionStart(), &offset));
        CHECK(uClass != UnitRole::FREE_UNITS);
        pageOwners.put(offset, nUnit * UNIT_SIZE, this);
    }

} // namespace MapleRuntime
#endif

namespace MapleRuntime {
inline ZGenerationId RegionInfo::generation_id() const { return metadata._generation_id; }
}

#include "Heap/Allocator/RegionInfo.h"

namespace MapleRuntime {
inline Generation RegionInfo::GetOwnerGeneration() const
    {
        return IsYoungRegion() ? Generation::Young : Generation::Old;
    }
}

namespace MapleRuntime {
inline bool RegionInfo::IsYoungRegion() const
    {
        return metadata.regionStateBitField.GetAtomicValue(RegionStateBitPos::YOUNG_REGION_FLAG, 1) != 0;
    }
}

namespace MapleRuntime {
inline MAddress RegionInfo::GetRegionStart() const { return GetUnitAddress(GetUnitIdx()); }
}

namespace MapleRuntime {
inline MAddress RegionInfo::GetRegionEnd() const { return metadata.regionEnd; }
}

namespace MapleRuntime {
inline MAddress RegionInfo::GetRegionAllocPtr() const { return metadata.allocPtr; }
}

namespace MapleRuntime {
inline bool RegionInfo::IsSmallRegion() const { return static_cast<UnitRole>(metadata.unitRole) == UnitRole::SMALL_SIZED_UNITS; }
}

namespace MapleRuntime {
inline bool RegionInfo::IsLargeRegion() const { return static_cast<UnitRole>(metadata.unitRole) == UnitRole::LARGE_SIZED_UNITS; }
}
