// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "MutatorManager.h"

#include <atomic>
#include <thread>
#include <cstdlib>
#include <cstring>
#include "Base/TimeUtils.h"
#include "Common/Runtime.h"
#include "Concurrency/ConcurrencyModel.h"
#include "Heap/Collector/FinalizerProcessor.h"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/WCollector/WCollector.h"
#include "Handshake.h"
#include "Mutator.inline.h"
#include "Heap/z/zStackWatermark.hpp"
#include "schedule.h"
#include "Loader/PackageInit.h"
#include "CpuProfiler/CpuProfiler.h"

namespace MapleRuntime {
// Mutator-list write-lock watchdog timeout (seconds). Read once from env
// cjMutatorLockTimeout, falling back to WAIT_LOCK_TIMEOUT, so heavy CPU-oversubscribed
// builds can raise it without a rebuild. A reader holding the list lock can be starved
// off-CPU far longer than the old fixed 30s under contention; failing fast there is a
// false positive, not a real deadlock.
static uint64_t GetWaitLockTimeoutSec()
{
    static const uint64_t timeout = []() -> uint64_t {
        const char* env = std::getenv("cjMutatorLockTimeout");
        if (env != nullptr) {
            char* end = nullptr;
            unsigned long long parsed = std::strtoull(env, &end, 10);
            if (end != env && parsed > 0) {
                return static_cast<uint64_t>(parsed);
            }
        }
        return WAIT_LOCK_TIMEOUT;
    }();
    return timeout;
}

extern "C" uintptr_t MRT_GetSafepointProtectedPage()
{
    return static_cast<uintptr_t>(true);
}

bool IsRuntimeThread()
{
    if (static_cast<int>(ThreadLocal::GetThreadType()) >= static_cast<int>(ThreadType::GC_THREAD)) {
        return true;
    }
    return false;
}

bool IsGcThread()
{
    if (static_cast<int>(ThreadLocal::GetThreadType()) == static_cast<int>(ThreadType::GC_THREAD)) {
        return true;
    }
    return false;
}

extern "C" void HandleSafepoint(ThreadLocalData* tlData)
{
    Handshake::Current().process_by_self();
    Mutator* mutator = tlData->mutator;
    if (mutator != nullptr) {
        mutator->DoEnterSaferegion();
        mutator->DoLeaveSaferegion();
    }
    UpdatePollValues(tlData);
    DLOG(SIGNAL, "HandleSafepoint, thread restarted.");
}

#if defined (__arm__)
extern "C" void HandleSafepointForArm(ThreadLocalData* tlData)
{
    if (tlData->safepointState == 0) {
        return;
    }
    Handshake::Current().process_by_self();
    Mutator* mutator = tlData->mutator;
    if (mutator != nullptr) {
        mutator->DoEnterSaferegion();
        mutator->DoLeaveSaferegion();
    }
    UpdatePollValues(tlData);
    DLOG(SIGNAL, "HandleSafepoint, thread restarted.");
}
#endif

void MutatorManager::BindMutator(Mutator& mutator) const
{
    ThreadLocalData* tlData = ThreadLocal::GetThreadLocalData();
    if (UNLIKELY(tlData->buffer == nullptr)) {
        (void)AllocBuffer::GetOrCreateAllocBuffer();
    }
    MutatorManager::Instance().RegisterMarkFlushThread(tlData);
    tlData->SetMutator(&mutator);
    mutator.SetAllocBuffer(tlData->buffer);
    UpdatePollValues(tlData);
}

void MutatorManager::UnbindMutator(Mutator& mutator) const
{
    ThreadLocalData* tlData = ThreadLocal::GetThreadLocalData();
    MRT_ASSERT(tlData->mutator == &mutator, "mutator in ThreadLocalData doesn't match in cjthread");
    tlData->SetMutator(nullptr);
    UpdatePollValues(tlData);
}

Mutator* MutatorManager::CreateMutator()
{
    Mutator* mutator = ConcurrencyModel::GetMutator();
    if (mutator == nullptr) {
        mutator = new (std::nothrow) Mutator();
        CHECK_DETAIL(mutator != nullptr, "new Mutator failed");
        MutatorManagementRLock();
        mutator->Init();
        mutator->InitTid();
        BindMutator(*mutator);
        ConcurrencyModel::SetMutator(mutator);
    } else {
        MutatorManagementRLock();
        mutator->Init();
        mutator->InitTid();
        BindMutator(*mutator);
    }
    MutatorManagementRUnlock();
    return mutator;
}

void MutatorManager::TransitMutatorToExit()
{
    Mutator* mutator = Mutator::GetMutator();
    CHECK_DETAIL(mutator != nullptr, "Mutator has not initialized or has been fini: %p", mutator);
    PackageInitTable::OwnerExit();
    mutator->MutatorLock();
    mutator->MutatorUnlock();
    mutator->ResetMutator();
    (void)mutator->EnterSaferegion(false);
    UnbindMutator(*mutator);
}

void MutatorManager::DestroyExpiredMutators()
{
    expiringMutatorListLock.lock();
    ExpiredMutatorList workList;
    workList.swap(expiringMutators);
    expiringMutatorListLock.unlock();
    for (auto it = workList.begin(); it != workList.end(); ++it) {
        Mutator* expiringMutator = *it;
        delete expiringMutator;
    }
}

void MutatorManager::DestroyMutator(Mutator* mutator)
{
    ConsumeCpuProfileRequest(mutator);
    if (TryAcquireMutatorManagementRLock()) {
        delete mutator; // call ~Mutator() under mutatorListLock
        MutatorManagementRUnlock();
    } else {
        expiringMutatorListLock.lock();
        expiringMutators.push_back(mutator);
        expiringMutatorListLock.unlock();
    }
}

Mutator* MutatorManager::CreateRuntimeMutator(ThreadType threadType)
{
    // Because TSAN tool can't identify the RwLock implemented by ourselves,
    // we use a global instance fpMutatorInstance instead of an instance created on
    // heap in order to prevent false positives.
    static Mutator fpMutatorInstance;
    Mutator* mutator = nullptr;
    if (threadType == ThreadType::FP_THREAD) {
        mutator = &fpMutatorInstance;
    } else {
        mutator = new (std::nothrow) Mutator();
    }
    CHECK_DETAIL(mutator != nullptr, "create mutator out of native memory");
    MutatorManagementRLock();
#ifdef INTERPRETER_ENABLED
    mutator->markAsRuntimeMutator();
#endif
    mutator->Init();
    mutator->InitTid();
    mutator->InitProtectStackAddr();
    mutator->SetManagedContext(false);
    MutatorManager::Instance().BindMutator(*mutator);
    {
        std::lock_guard<std::mutex> lock(runtimeMutatorRegistryMutex);
        runtimeMutators.insert(mutator);
    }
    ThreadLocal::SetMutator(mutator);
    ThreadLocal::SetThreadType(threadType);
    ThreadLocal::SetCJProcessorFlag(true);
    MutatorManagementRUnlock();
    if (threadType == ThreadType::UNCOMMITTER_THREAD) {
        // ZUncommitter joins the suspendible set only for allocator accounting.
        // This native participant has no CJThread or managed stack.
        return mutator; // initially in saferegion; ScopedObjectAccess joins STW
    }
    ThreadLocalData* threadData = reinterpret_cast<ThreadLocalData*>(MRT_GetThreadLocalData());
    // Managed-entry setup may block on sync/STW, so do not hold the mutator
    // management lock across it.
#if defined(MRT_TESTABLE_INTERNALS)
    // The lifecycle contract tests run without a scheduler-owned managed frame.
    // Stop after the real registration/born-clean path; product builds do not
    // contain this branch and all normal runtime mutators still enter managed code.
    if (std::getenv("MRT_GC_UNIT_RUNTIME_MUTATOR_LIFECYCLE_ONLY") != nullptr) {
        return mutator;
    }
#endif
    MRT_PreRunManagedCode(mutator, 2, threadData); // 2 layers
    // only running mutator can enter saferegion.
    return mutator;
}

void MutatorManager::DestroyRuntimeMutator(ThreadType threadType)
{
    Mutator* mutator = ThreadLocal::GetMutator();
    CHECK_DETAIL(mutator != nullptr, "Fini UpdateThreads with null mutator");

    // Reuse the ordinary CJThread exit transition: an active participant first
    // acknowledges as EXITING, is reset/unbound, and its storage remains pinned
    // by DestroyMutator until the epoch closes.
    TransitMutatorToExit();
    {
        std::lock_guard<std::mutex> lock(runtimeMutatorRegistryMutex);
        runtimeMutators.erase(mutator);
    }
    ThreadLocalData* tls = ThreadLocal::GetThreadLocalData();
    UnregisterMarkFlushThread(tls);
    ThreadLocal::SetAllocBuffer(nullptr);
    ThreadLocal::SetCJProcessorFlag(false);
    if (threadType != ThreadType::FP_THREAD) {
        DestroyMutator(mutator);
    }
}

void MutatorManager::Init()
{
#if defined(__linux__) || defined(hongmeng) || defined(__APPLE__)
    safepointPageManager = new (std::nothrow) SafepointPageManager();
    CHECK_DETAIL(safepointPageManager != nullptr, "new safepointPageManager failed");
    safepointPageManager->Init();
#endif
}

MutatorManager& MutatorManager::Instance() noexcept { return Runtime::Current().GetMutatorManager(); }

bool MutatorManager::ConcurrentStackScanEnabled()
{
    // Young and old marking share the required stack-watermark protocol.
    // ZGC does not retain a runtime configuration without concurrent roots.
    return true;
}

#if defined(MRT_TESTABLE_INTERNALS)
size_t MutatorManager::RuntimeMutatorRegistrySizeForTest()
{
    std::lock_guard<std::mutex> lock(runtimeMutatorRegistryMutex);
    return runtimeMutators.size();
}
#endif

void MutatorManager::AcquireMutatorManagementWLock()
{
    // Announce the pending writer so readers back off (writer-preference), then spin on
    // the non-blocking write-lock acquisition. Without this, sustained mutator-list
    // reader churn (many cjthreads registering/unregistering under heavy parallel
    // compilation) keeps the lock count above zero and starves this acquisition until
    // the watchdog below fires a false-positive "deadlock".
    AnnounceMgmtWriterPending();
    uint64_t start = TimeUtil::NanoSeconds();
    bool acquired = TryAcquireMutatorManagementWLock();
    while (!acquired) {
        TimeUtil::SleepForNano(WAIT_LOCK_INTERVAL);
        acquired = TryAcquireMutatorManagementWLock();
        uint64_t now = TimeUtil::NanoSeconds();
        if (!acquired && ((now - start) / SECOND_TO_NANO_SECOND > GetWaitLockTimeoutSec())) {
            LOG(RTLOG_FATAL, "Wait mutator list lock timeout");
        }
    }
    WithdrawMgmtWriterPending();
}

bool MutatorManager::AcquireMutatorManagementWLockForCpuProfile()
{
    AnnounceMgmtWriterPending();
    uint64_t start = TimeUtil::NanoSeconds();
    bool acquired = TryAcquireMutatorManagementWLock();
    while (!acquired) {
        TimeUtil::SleepForNano(WAIT_LOCK_INTERVAL);
        acquired = TryAcquireMutatorManagementWLock();
        uint64_t now = TimeUtil::NanoSeconds();
        if (!acquired && ((now - start) / SECOND_TO_NANO_SECOND > GetWaitLockTimeoutSec())) {
            LOG(RTLOG_FATAL, "Wait mutator list lock timeout");
        }
        if (!CpuProfiler::GetInstance().GetGenerator().GetIsStart()) {
            break;
        }
    }
    WithdrawMgmtWriterPending();
    return acquired;
}

// Visit all mutators, hold mutatorListLock firstly
void MutatorManager::VisitAllMutators(MutatorVisitor func)
{
    std::unordered_set<Mutator*> visited;
    MutatorVisitor visitOnce = [&func, &visited](Mutator& mutator) {
        if (visited.insert(&mutator).second) {
            func(mutator);
        }
    };
    ScheduleAllCJThreadVisitMutator(VisitMuatorHelper, &visitOnce);
    {
        std::lock_guard<std::mutex> lock(runtimeMutatorRegistryMutex);
        for (Mutator* runtimeMutator : runtimeMutators) {
            if (runtimeMutator != nullptr) {
                visitOnce(*runtimeMutator);
            }
        }
    }
    Mutator* mutator = Heap::GetHeap().GetFinalizerProcessor().GetMutator();
    if (mutator != nullptr) {
        visitOnce(*mutator);
    }
}

void MutatorManager::VisitAllMutatorsExceptFinalizer(MutatorVisitor func)
{
    ScheduleAllCJThreadVisitMutator(VisitMuatorHelper, &func);
}



void MutatorManager::VisitMarkingThreads(const std::function<void(const ThreadGCData*)>& visitor)
{
    DCHECK(WorldStopped());
    ThreadGCData::VisitOwners([&](ThreadGCData& data, Mutator*, ThreadLocalData*) {
        visitor(&data);
    });
}

void MutatorManager::VisitStoreBarrierBuffers(const std::function<void(MAddress)>& visitor)
{
    DCHECK(WorldStopped());
    ThreadGCData::VisitOwners([&](ThreadGCData& data, Mutator*, ThreadLocalData*) {
        StoreBarrierBuffer* buf = data.storeBarrierBuffer;
        for (size_t i = buf->Current(); i < kStoreBarrierBufferLength; ++i) {
            visitor(buf->buffer[i].p);
        }
    });
}

bool MutatorManager::StoreBarrierBufferContains(MAddress slot)
{
    bool found = false;
    VisitStoreBarrierBuffers([&](MAddress p) { found = found || p == slot; });
    return found;
}

HandshakeState* MutatorManager::HandshakeStateForTls(ThreadLocalData* tls)
{
    if (tls == nullptr) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(markFlushThreadMutex);
    auto it = markFlushThreads.find(tls);
    if (it == markFlushThreads.end()) {
        return nullptr;
    }
    return it->second->handshake;
}

void MutatorManager::EnqueueHandshakeOnAll(HandshakeClosure* cl, std::list<HandshakeOperation*>& ops,
                                           std::vector<MarkFlushThread*>& handle)
{
    std::lock_guard<std::mutex> lock(markFlushThreadMutex);
    for (auto& kv : markFlushThreads) {
        if (kv.first == nullptr || kv.second->dying.load(std::memory_order_acquire) != 0) {
            continue;
        }
        if (kv.first->mutator == nullptr) {
            continue;
        }
        HandshakeState* state = kv.second->handshake;
        if (state == nullptr) {
            kv.second->ownedHandshake = std::make_unique<HandshakeState>(kv.first);
            kv.second->handshake = kv.second->ownedHandshake.get();
            state = kv.second->handshake;
        }
        kv.second->refs.fetch_add(1, std::memory_order_acq_rel);
        handle.push_back(kv.second.get());
        auto* op = new HandshakeOperation(cl, kv.first);
        state->add_operation(op);
        ops.push_back(op);
    }
}

void MutatorManager::EnqueueHandshakeOn(ThreadLocalData* target, HandshakeClosure* cl,
                                        std::list<HandshakeOperation*>& ops, std::vector<MarkFlushThread*>& handle)
{
    if (target == nullptr || cl == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(markFlushThreadMutex);
    auto it = markFlushThreads.find(target);
    if (it == markFlushThreads.end() || it->second->dying.load(std::memory_order_acquire) != 0) {
        return;
    }
    HandshakeState* state = it->second->handshake;
    if (state == nullptr) {
        it->second->ownedHandshake = std::make_unique<HandshakeState>(it->first);
        it->second->handshake = it->second->ownedHandshake.get();
        state = it->second->handshake;
    }
    it->second->refs.fetch_add(1, std::memory_order_acq_rel);
    handle.push_back(it->second.get());
    auto* op = new HandshakeOperation(cl, it->first);
    state->add_operation(op);
    ops.push_back(op);
}

void MutatorManager::ReleaseHandshakeHandle(std::vector<MarkFlushThread*>& handle)
{
    for (MarkFlushThread* target : handle) {
        if (target != nullptr) {
            target->refs.fetch_sub(1, std::memory_order_acq_rel);
        }
    }
    handle.clear();
}

void MutatorManager::DemandSuspensionForSync()
{
    VisitAllMutators([](Mutator& mutator) {
        mutator.SetSuspensionFlag(Mutator::SuspensionType::SUSPENSION_FOR_SYNC);
    });
    ArmAllThreadPolls();
    class SyncHandshakeClosure : public HandshakeClosure {
    public:
        SyncHandshakeClosure() : HandshakeClosure("STW") {}
        void do_thread(ThreadLocalData* tls) override { (void)tls; }
    } cl;
    Handshake::execute(&cl);
}

bool MutatorManager::TlsObservedSafe(ThreadLocalData* tls)
{
    HandshakeState* state = HandshakeStateForTls(tls);
    if (state == nullptr) {
        return tls == ThreadLocal::GetThreadLocalData();
    }
    return state->observed_safe();
}

void MutatorManager::RegisterMarkFlushThread(ThreadLocalData* tls)
{
    if (tls == ThreadLocal::GetThreadLocalData()) {
        ThreadLocal::InitializeCleaner();
    }
    // GC workers rendezvous at task boundaries, not through mutator handshakes.
    if (tls == nullptr || tls->threadType == ThreadType::GC_THREAD) {
        return;
    }
    std::lock_guard<std::mutex> lock(markFlushThreadMutex);
    auto& slot = markFlushThreads[tls];
    if (slot == nullptr) {
        slot = std::make_unique<MarkFlushThread>();
        slot->tls = tls;
        slot->ownedHandshake = std::make_unique<HandshakeState>(tls);
        slot->handshake = slot->ownedHandshake.get();
    }
    if (slot->handshake == nullptr) {
        slot->ownedHandshake = std::make_unique<HandshakeState>(tls);
        slot->handshake = slot->ownedHandshake.get();
    }
    slot->handshake->set_handshakee(tls);
    Handshake::BindCurrent(slot->handshake);
    slot->dying.store(0, std::memory_order_release);
    slot->bufferLive.store(1, std::memory_order_release);
}

void MutatorManager::UnregisterMarkFlushThread(ThreadLocalData* tls)
{
    if (tls == nullptr) {
        return;
    }
    MarkFlushThread* target = nullptr;
    {
        std::lock_guard<std::mutex> lock(markFlushThreadMutex);
        auto it = markFlushThreads.find(tls);
        if (it == markFlushThreads.end()) {
            return;
        }
        target = it->second.get();
        target->dying.store(1, std::memory_order_release);
        target->refs.fetch_add(1, std::memory_order_acq_rel);
    }
    HandshakeState* hs = target->handshake;
    if (hs != nullptr) {
        hs->process_queued_then_detach([](ThreadLocalData* t) { (void)ZMark::FlushThread(t); });
    } else {
        (void)ZMark::FlushThread(tls);
    }
    target->bufferLive.store(0, std::memory_order_release);
    target->refs.fetch_sub(1, std::memory_order_acq_rel);
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(markFlushThreadMutex);
            auto it = markFlushThreads.find(tls);
            if (it == markFlushThreads.end()) {
                break;
            }
            if (it->second->refs.load(std::memory_order_acquire) == 0) {
                markFlushThreads.erase(it);
                break;
            }
        }
        std::this_thread::yield();
    }
    if (tls == ThreadLocal::GetThreadLocalData()) {
        Handshake::BindCurrent(nullptr);
    }
}

bool MutatorManager::TlsHasMarkFlushPending(ThreadLocalData* tls)
{
    HandshakeState* state = Handshake::ForTls(tls);
    return state != nullptr && state->has_operation();
}

bool MutatorManager::AcknowledgeMarkFlushForCurrentThread()
{
    const bool pending = Handshake::Current().has_operation();
    Handshake::Current().process_by_self();
    return pending;
}

void MutatorManager::StopTheWorld()
{
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    bool saferegionEntered = false;
    if (!IsGcThread()) {
        Mutator* mutator = Mutator::GetMutator();
        if (mutator != nullptr) {
            saferegionEntered = mutator->EnterSaferegion(true);
        }
    }
#endif
    syncMutex.lock();
    syncTriggered.store(true);

    AcquireMutatorManagementWLock();

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    saferegionStateChanged = saferegionEntered;
#endif

    size_t mutatorCount = GetMutatorCount();
    if (UNLIKELY(mutatorCount == 0)) {
        worldStopped.store(true, std::memory_order_release);
        return;
    }
    SetSuspensionMutatorCount(static_cast<uint32_t>(mutatorCount));
    DemandSuspensionForSync();
    WaitUntilAllMutatorStopped();

    worldStopped.store(true, std::memory_order_release);
}

void MutatorManager::StartTheWorld() noexcept
{
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    bool shouldLeaveSaferegion = saferegionStateChanged;
#endif
    syncTriggered.store(false);
    worldStopped.store(false, std::memory_order_release);

    CancelSuspensionAfterSync();
    SetSuspensionMutatorCount(0);

    // wakeup all mutators which blocking on countOfMutatorsToStop futex.
#if defined(_WIN64) || defined(__APPLE__)
    WakeAllMutators();
#else
    (void)MapleRuntime::Futex(GetSyncFutexWord(), FUTEX_WAKE, INT_MAX);
#endif

    MutatorManagementWUnlock();

    // Release syncMutex to allow other thread call STW.
    syncMutex.unlock();
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    // Restore saferegion state if the state is changed when mutator calls StopTheWorld().
    if (!IsGcThread()) {
        Mutator* mutator = Mutator::GetMutator();
        if (mutator != nullptr && shouldLeaveSaferegion) {
            (void)mutator->LeaveSaferegion();
        }
    }
#endif
}

void MutatorManager::StartLightSync()
{
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    bool saferegionEntered = false;
    if (!IsGcThread()) {
        Mutator* mutator = Mutator::GetMutator();
        if (mutator != nullptr) {
            saferegionEntered = mutator->EnterSaferegion(true);
        }
    }
#endif
    syncMutex.lock();
    syncTriggered.store(true);

    AcquireMutatorManagementWLock();

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    saferegionStateChanged = saferegionEntered;
#endif

    size_t mutatorCount = GetMutatorCount();
    if (UNLIKELY(mutatorCount == 0)) {
        worldStopped.store(true, std::memory_order_release);
    } else {
        SetSuspensionMutatorCount(static_cast<uint32_t>(mutatorCount));
        DemandSuspensionForSync();
        WaitUntilAllMutatorStopped();
        worldStopped.store(true, std::memory_order_release);
    }
}

void MutatorManager::StopLightSync() noexcept
{
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    bool shouldLeaveSaferegion = saferegionStateChanged;
#endif

    syncTriggered.store(false);
    worldStopped.store(false, std::memory_order_release);

    CancelSuspensionAfterSync();
    SetSuspensionMutatorCount(0);

    // wakeup all mutators which blocking on countOfMutatorsToStop futex.
#if defined(_WIN64) || defined(__APPLE__)
    WakeAllMutators();
#else
    (void)MapleRuntime::Futex(GetSyncFutexWord(), FUTEX_WAKE, INT_MAX);
#endif
    MutatorManagementWUnlock();
    // Release syncMutex to allow other thread call lsync.
    syncMutex.unlock();
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    // Restore saferegion state if the state is changed when mutator calls StartLightSync().
    if (!IsGcThread()) {
        Mutator* mutator = Mutator::GetMutator();
        if (mutator != nullptr && shouldLeaveSaferegion) {
            (void)mutator->LeaveSaferegion();
        }
    }
#endif
}

void MutatorManager::WaitUntilAllMutatorStopped()
{
    uint64_t beginTime = TimeUtil::MilliSeconds();
    std::list<Mutator*> unstoppedMutators;
    auto func = [&unstoppedMutators](Mutator& mutator) {
        if ((!mutator.InSaferegion())) {
            unstoppedMutators.emplace_back(&mutator);
        }
    };
    VisitAllMutators(func);

    size_t remainMutatorsSize = unstoppedMutators.size();
    if (remainMutatorsSize == 0) {
        return;
    }

    // Synchronize operation to ensure that all mutators complete phase transition
    // Use unstoppedMutators to avoid traversing the entire mutatorList
    int timeoutTimes = 0;
    while (true) {
        for (auto it = unstoppedMutators.begin(); it != unstoppedMutators.end();) {
            Mutator* mutator = *it;
            if (mutator->InSaferegion()) {
                // current it(mutator) is finished by GC
                it = unstoppedMutators.erase(it);
            } else {
                ++it; // skip current round & check it next round
            }
        }

        if (unstoppedMutators.size() == 0) {
            return;
        }

        if (UNLIKELY(TimeUtil::MilliSeconds() - beginTime >
            (((remainMutatorsSize / STW_TIMEOUTS_THREADS_BASE_COUNT) * STW_TIMEOUTS_BASE_MS) + STW_TIMEOUTS_BASE_MS))) {
            timeoutTimes++;
            beginTime = TimeUtil::MilliSeconds();
            DumpMutators(timeoutTimes);
        }

        (void)sched_yield();
    }
}



void MutatorManager::TransitionAllMutatorsToCpuProfile()
{
    bool worldStopped = WorldStopped();
    if (!worldStopped) {
        if (!AcquireMutatorManagementWLockForCpuProfile()) {
            return;
        }
    }
    VisitAllMutatorsExceptFinalizer([](Mutator& mutator) {
        if (mutator.GetCjthreadPtr() == MutatorManager::Instance().GetMainThreadHandle()) {
            PublishCpuProfileRequest(&mutator);
        }
    });
    if (!worldStopped) {
        MutatorManagementWUnlock();
    }
}

void MutatorManager::DumpMutators(uint32_t timeoutTimes)
{
    constexpr size_t bufferSize = 4096;
    char buf[bufferSize];
    int index = 0;
    size_t visitedCount = 0;
    size_t visitedSaferegion = 0;
    int firstNotStoppedTid = -1;
    index += sprintf_s(buf, sizeof(buf), "not stopped: ");
    CHECK_DETAIL(index != -1, "Dump mutators state failed");
    size_t mutatorCount = 0;
    VisitAllMutators([&](const Mutator& mut) {
        mutatorCount++;
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
        mut.DumpMutator();
#endif
        if (!mut.InSaferegion()) {
            if (firstNotStoppedTid == -1) {
                firstNotStoppedTid = static_cast<int>(mut.GetTid());
            }
            int ret = sprintf_s(buf + index, sizeof(buf) - index, "%u ", mut.GetTid());
            CHECK_DETAIL(ret != -1, "Dump mutators state failed");
            index += ret;
        } else {
            ++visitedSaferegion;
        }
        ++visitedCount;
    });
    LOG(RTLOG_ERROR, "MutatorList size : %zu", mutatorCount);

    CHECK_DETAIL(sprintf_s(buf + index, sizeof(buf) - index, ", total: %u, visited: %zu/%zu",
                           GetSuspensionMutatorCount(), visitedSaferegion, visitedCount) != -1,
                 "Dump mutators state failed");
    CHECK_DETAIL(timeoutTimes <= MAX_TIMEOUT_TIMES, "Waiting mutators entering saferegion timeout status info:%s", buf);
    LOG(RTLOG_ERROR, "STW status info:%s", buf);
}

#if defined(GCINFO_DEBUG) && GCINFO_DEBUG
void MutatorManager::DumpForDebug()
{
    size_t count = 0;
    auto func = [&count](Mutator& mutator) {
        mutator.DumpMutator();
        count++;
    };
    VisitAllMutators(func);
    LOG(RTLOG_INFO, "MutatorList size : %zu", count);
}

void MutatorManager::DumpAllGcInfos()
{
    auto func = [](Mutator& mutator) { mutator.DumpGCInfos(); };
    VisitAllMutators(func);
}
#endif

extern "C" void MRT_FlushGCInfo()
{
#if defined(GCINFO_DEBUG) && GCINFO_DEBUG
    // MutatorManager::Instance().DumpAllGcInfos();
    Mutator::GetMutator()->DumpGCInfos();
#endif
}

void MarkFlushOnEnterSaferegion()
{
    Handshake::Current().enter_safe();
}

void MarkFlushBeginLeaveSaferegion()
{
    Handshake::Current().leave_safe();
}

void MarkFlushEndLeaveSaferegion()
{
}

bool MarkFlushPendingForCurrentThread()
{
    return Handshake::Current().has_operation();
}

void RegisterCurrentMarkFlushThread()
{
    MutatorManager::Instance().RegisterMarkFlushThread(ThreadLocal::GetThreadLocalData());
}

#ifdef __APPLE__
extern "C" MRT_EXPORT void CJ_MRT_FlushGCInfo();
__asm__(".global _CJ_MRT_FlushGCInfo\n\t.set _CJ_MRT_FlushGCInfo, _MRT_FlushGCInfo");
#else
extern "C" MRT_EXPORT void CJ_MRT_FlushGCInfo() __attribute__((alias("MRT_FlushGCInfo")));
#endif
} // namespace MapleRuntime
