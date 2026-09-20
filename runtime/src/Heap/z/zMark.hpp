// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#ifndef MRT_MARKING_STACKS_H
#define MRT_MARKING_STACKS_H
#include <cstddef>
#include <cstdint>
namespace MapleRuntime {
class ZMark;
namespace MarkingStacks {
enum class MarkingGeneration : uint8_t { MAJOR, YOUNG };
}
}
#endif

// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_MARK_ENGINE_H
#define MRT_MARK_ENGINE_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "Heap/z/RegionSpace.h"
#include "Heap/z/zForwardingTable.hpp"

#include "Heap/z/zMarkStack.hpp"
#include "Common/MarkWorkStack.h"
#include "Heap/z/zCrossVM.hpp"
#include "Heap/z/zAbort.hpp"
#include "Heap/z/zBarrier.hpp"
#include "Heap/z/zAddress.hpp"
#include "Heap/z/zMarkingSMR.hpp"
#include "Heap/z/zMarkTerminate.hpp"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zHeap.hpp"

namespace MapleRuntime {

class MarkStripeSet;
class ZWorkers;
class MarkContext;
class AllocBuffer;
struct ThreadGCData;
struct ThreadLocalData;
class Mutator;
struct YoungConcWindowStats;

// Per-generation mark ownership (zMark.hpp:42-124, zMark.cpp:80-92).
class ZMark {
    friend class ZMarkTask;
public:
    static void VisitStrongPlainRoots(const RootVisitor& visitor,
                              const std::function<void(Mutator&)>& threadVisitor);
    static void DiscoverWeakReference(BaseObject* reference, WorkStack& workStack);
    static void EnumAllCommonRoots(ZWorkers& workers);
    static void EnumAllExportRoots(ValueRootList& exportOwners);
    static void DiscoverFinalizableRoot(NativeSlot& slot);
    static void MergeMutatorRoots(WorkStack& workStack);
    static void DoEnumeration(WorkStack& workStack, ValueRootList& exportOwners);
    static void VisitStaticRoots(const NativeSlotVisitor& visitor);
    static void EnumRefFieldRoot(RefField<>& ref, ValueRootList& exportOwners);
    static void ProcessFinalizers();
    static void VisitMinorRootSlots(RootVisitor& rawRootVisitor, RootVisitor& invisibleRootVisitor,
                             uint64_t stackScanEpoch = 0);
    static void VisitMinorRoots(const std::function<void(BaseObject*)>& visitor,
                         const std::function<void(BaseObject*)>& invisibleVisitor,
                         uint64_t stackScanEpoch = 0);
    static void PushYoungObject(BaseObject* object, WorkStack& workStack, const char* origin = "unknown");
    static void PushYoungObject(BaseObject* object, WorkStack& workStack, const char* origin, bool finalizable);
    static void TraceYoungClosure(WorkStack& workStack, bool fullYoungScan,
                           std::vector<BaseObject*>& reachableVec, std::unordered_set<MAddress>& reachableSlots,
                           std::unordered_set<MAddress>& weakSlots,
                           const std::unordered_set<MAddress>* reachableSlotDomain = nullptr);
    static void TraceYoungClosureStriped(WorkStack& workStack, bool fullYoungScan,
                                  std::vector<BaseObject*>& reachableVec, std::unordered_set<MAddress>& reachableSlots,
                                  std::unordered_set<MAddress>& weakSlots,
                                  const std::unordered_set<MAddress>* reachableSlotDomain = nullptr);
    static bool FollowYoungMark(WorkStack& workStack, bool fullYoungScan,
                             std::vector<BaseObject*>& reachableVec, std::unordered_set<MAddress>& reachableSlots,
                             std::unordered_set<MAddress>& weakSlots,
                             YoungConcWindowStats* windowStats = nullptr);
    static bool TryEndYoungMark(WorkStack& workStack, YoungConcWindowStats* windowStats = nullptr);

    static bool PublishHandshakeMarkWork(WorkStack& work, ZMark* domain);
    static bool FlushThreadMarkProducers(ThreadLocalData* tls, ZMark* domain);
    static bool FlushThreadMarkProducers(ThreadLocalData* tls);
    static bool FlushGCDataMarkProducers(ThreadGCData& data, ZMark* domain);
    static bool FlushGCDataMarkProducers(ThreadGCData& data);
    static constexpr bool Resurrect = true;
    static constexpr bool DontResurrect = false;
    static constexpr bool GCThread = true;
    static constexpr bool AnyThread = false;
    static constexpr bool Follow = true;
    static constexpr bool DontFollow = false;
    static constexpr bool Strong = false;
    static constexpr bool Finalizable = true;

    enum class Result { Completed, Partial, Aborted };
    using Process = std::function<void(const MarkStackEntry&)>;

    explicit ZMark(size_t capacity, MarkingStacks::MarkingGeneration generation);
    template<bool resurrect, bool gcThread, bool follow, bool finalizable>
    void MarkObject(zaddress address);
    void Start();
    void PrepareWork();
    void PrepareWork(size_t nworkers);
    void ResizeWorkers(size_t nworkers);
    void FinishWork();
    void MarkFollow(bool partial = false);
    void FollowWorkComplete(bool partial);
    bool FollowWorkPartial();
    void MarkAndFollow(MarkContext& context, const MarkStackEntry& entry);
    static bool MarkEntryObject(BaseObject* obj, const MarkStackEntry& entry, MarkLiveCache* cache);
    void BindWorkers(ZWorkers* workers) { gcWorkers = workers; }
    bool PollStop();
    MarkStripeSet& Stripes() { return stripes; }
    MarkTerminate& Terminate() { return terminate; }
    MarkingSMR& Smr() { return smr; }
    MarkThreadLocalStacks& Stacks();
    size_t NWorkers() const { return nworkers; }
    size_t TargetNStripes() const { return targetNStripes; }
    bool Flush();
    bool Flush(ThreadLocalData* tls);
    static bool FlushThread(ThreadLocalData* tls);
    static bool FlushAllGenerations();
    bool FlushStacks();
    bool TryTerminateFlush();
    void verify_all_stacks_empty() const;
    void verify_worker_stacks_empty() const;
    bool TryProactiveFlush(size_t workerId);
    bool TryEnd();
    void Free();
    MarkingStacks::MarkingGeneration Generation() const { return generation; }

    static Result FollowWork(MarkContext& context, MarkingSMR& smr, MarkStripeSet& stripes,
                             MarkTerminate& terminate, size_t workerId, bool partial,
                             const Process& process, std::atomic<size_t>* stealSuccess = nullptr,
                             std::atomic<size_t>* stealFailure = nullptr, ZMark* domain = nullptr);

private:
    size_t CalculateNStripes(size_t nworkers) const;
    void EnsureWorkers(size_t nworkers);
    static bool HandshakeFlush(ZMark* domain);
    static bool FlushThreadLocal(ThreadLocalData* tls, ZMark* domain);

    MarkingSMR smr;
    MarkStripeSet stripes;
    MarkTerminate terminate;
    std::atomic<size_t> workNProactiveFlush{0};
    std::atomic<size_t> workNTerminateFlush{0};
    size_t nproactiveflush = 0;
    size_t nterminateflush = 0;
    size_t ntrycomplete = 0;
    size_t ncontinue = 0;
    size_t nworkers = 0;
    size_t targetNStripes = 0;
    MarkingStacks::MarkingGeneration generation;
    ZWorkers* gcWorkers = nullptr;
};

} // namespace MapleRuntime

#endif

// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_COLLECTOR_TRACING_H
#define MRT_COLLECTOR_TRACING_H

#include <atomic>
#include <cstdint>
#include <map>

#include "Heap/z/zCollectedHeap.hpp"

#include "Heap/z/zDriver.hpp"
#include "Common/MarkWorkStack.h"
#include "Heap/z/RegionSpace.h"
#include "Heap/z/zMarkStackEntry.hpp"
#include "Mutator/MutatorManager.h"

// set 1 to enable concurrent mark test.
#define MRT_TEST_CONCURRENT_MARK (false)

#include "Heap/z/zRootsIterator.hpp"
namespace MapleRuntime {
class MarkLiveCache;
// number of nanoseconds in a microsecond.
constexpr uint64_t NS_PER_US = 1000;
constexpr uint64_t NS_PER_S = 1000000000;

// Strict mark-end cut shared by major mark-end and young
// FollowYoungMark. ZMark::end -> try_end (zMark.cpp:954-971) decides
// termination with mutators stopped, after ZMark::flush (zMark.cpp:587-605,
// :998-1006), and resumes concurrent follow when that cut exposes work
// (zMark.cpp:973-990). This must stay compile-time and default-on: retired-only
// sampling cannot see a mutator's non-full SATB node.


// prefetch distance for mark.
#define MACRO_MARK_PREFETCH_DISTANCE 16    // this macro is used for check when pre-compiling.
constexpr int MARK_PREFETCH_DISTANCE = 16; // when it is changed, remember to change MACRO_MARK_PREFETCH_DISTANCE.

// Small queue implementation, for prefetching.
#define MRT_MAX_PREFETCH_QUEUE_SIZE_LOG 5UL
#define MRT_MAX_PREFETCH_QUEUE_SIZE (1UL << MRT_MAX_PREFETCH_QUEUE_SIZE_LOG)
#if MRT_MAX_PREFETCH_QUEUE_SIZE <= MACRO_MARK_PREFETCH_DISTANCE
#error Prefetch queue size must be strictly greater than prefetch distance.
#endif



// For managing gc roots


// portyoungconc: work accounting for the concurrent young mark window.
// ZGC anchor: ZGenerationYoung::concurrent_mark() = mark_roots() + mark_follow()
// (zGeneration.cpp:665-669) — everything a young collector does between
// pause_mark_start and pause_mark_end runs with mutators alive.
// Every field below counts GC work performed while the world is running. A
// Duration alone does not establish that the concurrent window performed marking work.







class VM_ZMarkStartYoung;
class VM_ZMarkStartYoungAndOld;
class VM_ZMarkEndYoung;
class VM_ZRelocateStartYoung;
class VM_ZMarkEndOld;
class VM_ZRelocateStartOld;
class VM_ZVerifyOld;


class MarkingWork;


} // namespace MapleRuntime
#endif // MRT_COLLECTOR_TRACING_H
