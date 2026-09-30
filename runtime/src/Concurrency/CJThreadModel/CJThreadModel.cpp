// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "CJThreadModel.h"

#include <pthread.h>
#include <thread>

#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Cki.h"
#include "Exception/ExceptionCApi.h"
#include "LoaderManager.h"
#include "Mutator/MutatorManager.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif
#include "schedule.h"
#include "Heap/z/zUncoloredRoot.inline.hpp"

namespace MapleRuntime {
namespace {
thread_local uintptr_t* g_uncoloredVisitColor = nullptr;
}

extern "C" uintptr_t* MRT_BindUncoloredVisitColor(uintptr_t* slot)
{
    auto* previous = g_uncoloredVisitColor;
    g_uncoloredVisitColor = slot;
    return previous;
}

namespace {
// zBarrierSetNMethod.cpp:53-91: mutator entry slow path. Runs under the group
// lock via CJThreadVisitRoots; heals with process_weak (keep-alive) and fully
// disarms by publishing the store-good guard.
void MutatorEntryCaller(void* argPtr, void* handle)
{
    CJThreadRoot root(*static_cast<LWTData*>(argPtr), *g_uncoloredVisitColor);
    root.entry_barrier();
    if (handle != nullptr) {
        root.oops_do(*static_cast<RootVisitor*>(handle));
    }
}
} // namespace

bool CJThreadRoot::is_armed() const
{
    return color != ZPointerStoreGoodMask;
}

void CJThreadRoot::oops_do(const RootVisitor& visitor)
{
    visitor(RootSlotAt(&data.obj));
    visitor(RootSlotAt(&data.threadObject));
    visitor(RootSlotAt(&data.execute));
}

void CJThreadRoot::entry_barrier()
{
    // ZGC zBarrierSetNMethod.cpp:53-97: recheck under the carrier lock.
    if (is_armed()) {
        ZUncoloredRootProcessWeakOopClosure closure(color);
        OopClosure& processWeak = closure;
        oops_do([&](RootSlot& slot) {
            processWeak.do_oop(&HeapSlotAt<>(static_cast<void*>(&slot)));
        });
        __atomic_store_n(&color, ZPointerStoreGoodMask, __ATOMIC_RELEASE);
    }
}

void VisitCJThreadRoots(const std::function<void(CJThreadRoot&)>& visitor)
{
    auto copy = visitor;
    ScheduleAllCJThreadVisit([](void* data, void* handle) {
        CJThreadRoot root(*static_cast<LWTData*>(data), *g_uncoloredVisitColor);
        (*static_cast<std::function<void(CJThreadRoot&)>*>(handle))(root);
    }, &copy);
}

void CJThreadRootEntryBarrier()
{
    // zBarrierSetNMethod.cpp:39-43: fast guard check against the disarmed
    // (store good) value; the GC partial color is still armed.
    auto thread = CJThreadGetHandle();
    if (!CJThreadRootsAreArmed(thread, ZPointerStoreGoodMask)) {
        return;
    }
    CJThreadVisitRoots(thread, MutatorEntryCaller, nullptr);
}

void StoreCJThreadObject(void* object)
{
    auto* data = static_cast<LWTData*>(CJThreadGetArg());
    RootVisitor store = [&](RootSlot& slot) {
        // zBarrierSetNMethod.cpp:76-79: a mutator entry keeps the old oops
        // alive before the guard is disarmed or a root is overwritten. When
        // the group is already disarmed this is the only keep-alive on store.
        ZUncoloredRoot::keep_alive_object(safe(slot.LoadPlain()));
        if (&slot == &RootSlotAt(&data->threadObject)) {
            StorePlain(slot, from_object(from_native_ref(object)));
        }
    };
    // zBarrierSetNMethod.cpp:78-84: a mutator entry heals the group with
    // process_weak before a root is overwritten; production and consumption
    // share this entry.
    CJThreadVisitRoots(CJThreadGetHandle(), MutatorEntryCaller, &store);
}

// External interface for adapting to concurrent tasks
extern "C" uintptr_t MRT_CreateMutator()
{
    Mutator* mutator = MutatorManager::Instance().CreateMutator();
    ThreadLocalData* threadData = reinterpret_cast<ThreadLocalData*>(MRT_GetThreadLocalData());
    MRT_PreRunManagedCode(mutator, 1, threadData); // one layer call chain
    return 0;
}

extern "C" uintptr_t MRT_TransitMutatorToExit()
{
    MutatorManager::Instance().TransitMutatorToExit();
    return 0;
}

extern "C" void MRT_DestroyMutator(void* mutator)
{
    MutatorManager::Instance().DestroyMutator(reinterpret_cast<Mutator*>(mutator));
}


static void RegisterCJThreadHooks()
{
    (void)CJThreadSchdHookRegister(MRT_StopGCWork, SCHD_STOP);
    (void)CJThreadSchdHookRegister(MRT_CreateMutator, SCHD_CREATE_MUTATOR);
    (void)CJThreadSchdHookRegister(MRT_TransitMutatorToExit, SCHD_DESTROY_MUTATOR);
    (void)CJThreadSchdHookRegister(MRT_GetSafepointProtectedPage, SCHD_PREEMPT_REQ);
    (void)CJThreadDestructorHookRegister(MRT_DestroyMutator);
    LogRegister(MRT_DumpLog, ENABLE_LOG(LogType::CJTHREAD), LogFile::GetLogLevel());
}

static bool GetStackGuardFlagEnv()
{
    const char* env = std::getenv("MRT_STACK_CHECK");
    if (env != nullptr) {
        if (CString::ParseFlagFromEnv(env)) {
            return true;
        }
        LOG(RTLOG_ERROR, "unsupported MRT_STACK_CHECK. Should set variable to 1 or true or TRUE\n");
    }
    return false;
}

// ConcurrencyParam.processorNum set the processor number of scheduler, it is set as following ways:
// 1. User can set the environment variable 'cjProcessorNum' firstly.
// 2. If the variable 'cjProcessorNum' is invalid, set it by return value of hardware_concurrency().
// 3. If not, use a default value of 8 to set it finally.
void CJThreadModel::Init(const ConcurrencyParam param, ScheduleType scheduleType)
{
    ScheduleAttr attr;
    stackGuardCheck = GetStackGuardFlagEnv();
    ScheduleAttrInitWithParams(&attr, param, stackGuardCheck, scheduleType != SCHEDULE_UI_THREAD);

    ScheduleGetTlsHookRegister((GetTlsHookFunc)MRT_GetThreadLocalData);

    // should not use system page size to calculate reserved stack size,
    // because the page size could be different in different system.
#ifdef _WIN64
    constexpr uint32_t reservedStackSize = 24 * KB;
#else
    constexpr uint32_t reservedStackSize = 8 * KB;
#endif
    CJThreadStackReversedSet(reservedStackSize);
    scheduler = ScheduleNew(scheduleType, &attr);
    Cki::CreateCKI();

#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanInitialize();
#endif
    RegisterCJThreadHooks();
}

void CJThreadModel::VisitGCRoots(RootVisitor* visitorHandle)
{
    // ZGC zHeapIterator.cpp:364-371: heap inspection enters the barrier
    // before observing the carrier's roots. GC tasks use their own closures.
    VisitCJThreadRoots([&](CJThreadRoot& root) {
        root.entry_barrier();
        root.oops_do(*visitorHandle);
    });
}

// Get current mutator from tls
Mutator* ConcurrencyModel::GetMutator()
{
    if (!IsRuntimeThread()) {
        return reinterpret_cast<Mutator*>(CJThreadGetMutator());
    } else {
        return ThreadLocal::GetMutator();
    }
}

void ConcurrencyModel::SetMutator(Mutator* mutator) { CJThreadSetMutator(mutator); }
} // namespace MapleRuntime
