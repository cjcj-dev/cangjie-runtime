// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Loader/ElfUnloadQuiescence.h"

#include <array>
#include <condition_variable>
#include <new>
#include <thread>

#include "Base/Panic.h"
#include "Common/ScopedObjectAccess.h"
#include "Mutator/MutatorManager.h"
#include "RuntimeConfig.h"

#ifdef _WIN64
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace MapleRuntime {
namespace {
constexpr size_t MAX_LINKED_IMAGES = 4096;
std::array<std::atomic<Uptr>, MAX_LINKED_IMAGES> linkedImages {};
thread_local U32 readerDepth = 0;
thread_local bool unloadWriter = false;
thread_local Uptr unloadWriterImage = 0;
thread_local Uptr purgeAuthorizedImage = 0;
thread_local const ElfUnloadQuiescence::TaskAdmissionScope* purgeAdmission = nullptr;
#ifdef MRT_TESTABLE_INTERNALS
std::atomic<U64> gcEntryCount { 0 };
std::atomic<U64> directEventSequence { 0 };
std::atomic<U64> directUnlinkEvent { 0 };
std::atomic<U64> directHandshakeEvent { 0 };
std::atomic<U64> directPurgeEvent { 0 };
std::atomic<bool> directPreflightEntered { false };
std::atomic<bool> gcReaderPauseEnabled { false };
std::atomic<bool> gcReaderPaused { false };
std::atomic<bool> gcReaderReleased { false };
std::atomic<bool> packageReaderPauseEnabled { false };
std::atomic<bool> packageReaderPaused { false };
std::atomic<bool> packageReaderReleased { false };
std::atomic<bool> unrelatedStwHeld { false };
std::atomic<bool> unrelatedStwReleased { false };
#endif
} // namespace

std::atomic<U64>& ElfUnloadQuiescence::State()
{
    static std::atomic<U64> state { 0 };
    return state;
}

std::mutex& ElfUnloadQuiescence::WriterMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::mutex& ElfUnloadQuiescence::DrainMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::condition_variable& ElfUnloadQuiescence::DrainCondition()
{
    static std::condition_variable condition;
    return condition;
}

std::shared_timed_mutex& ElfUnloadQuiescence::TaskAdmissionMutex()
{
    static std::shared_timed_mutex mutex;
    return mutex;
}

std::mutex& ElfUnloadQuiescence::PendingTaskMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::condition_variable& ElfUnloadQuiescence::PendingTaskCondition()
{
    static std::condition_variable condition;
    return condition;
}

std::unordered_set<ElfUnloadQuiescence::PendingTask*>& ElfUnloadQuiescence::PendingTasks()
{
    static std::unordered_set<PendingTask*> tasks;
    return tasks;
}

Uptr ElfUnloadQuiescence::ResolveImageIdentity(Uptr address)
{
    if (address == 0) {
        return 0;
    }
#ifdef _WIN64
    MEMORY_BASIC_INFORMATION info {};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) == 0) {
        return 0;
    }
    return reinterpret_cast<Uptr>(info.AllocationBase);
#else
    Dl_info info {};
    if (dladdr(reinterpret_cast<const void*>(address), &info) == 0 || info.dli_fbase == nullptr) {
        return 0;
    }
    return reinterpret_cast<Uptr>(info.dli_fbase);
#endif
}

ElfUnloadQuiescence::ReadScope::ReadScope(ReaderKind kind)
{
#ifdef MRT_TESTABLE_INTERNALS
    if (kind == ReaderKind::GC_STACK_ENTRY) {
        gcEntryCount.fetch_add(1, std::memory_order_relaxed);
    }
#else
    (void)kind;
#endif
    if (readerDepth != 0 || unloadWriter) {
        ++readerDepth;
        active = true;
        return;
    }

    auto& state = State();
    for (;;) {
        U64 observed = state.load(std::memory_order_acquire);
        if ((observed & WRITER_BIT) != 0) {
            std::this_thread::yield();
            continue;
        }
        CHECK_DETAIL((observed & READER_MASK) != READER_MASK, "ELF unload reader counter overflow");
        if (state.compare_exchange_weak(observed, observed + 1,
                                        std::memory_order_acquire, std::memory_order_relaxed)) {
            readerDepth = 1;
            active = true;
            return;
        }
    }
}

ElfUnloadQuiescence::ReadScope::~ReadScope()
{
    if (!active) {
        return;
    }
    CHECK_DETAIL(readerDepth != 0, "ELF unload reader depth underflow");
    if (--readerDepth != 0 || unloadWriter) {
        return;
    }
    U64 previous = State().fetch_sub(1, std::memory_order_release);
    CHECK_DETAIL((previous & READER_MASK) != 0, "ELF unload reader counter underflow");
    if ((previous & READER_MASK) == 1 && (previous & WRITER_BIT) != 0) {
        DrainCondition().notify_all();
    }
}

ElfUnloadQuiescence::UnloadScope::UnloadScope(Uptr imageAddress)
    : writerLock(WriterMutex()), imageIdentity(ResolveImageIdentity(imageAddress))
{
    CHECK_DETAIL(readerDepth == 0 && !unloadWriter,
                 "ELF unload cannot begin from inside a metadata reader");
    CHECK_DETAIL(imageIdentity != 0, "ELF unload image identity is unavailable for %p",
                 reinterpret_cast<void*>(imageAddress));
    U64 previous = State().fetch_or(WRITER_BIT, std::memory_order_acq_rel);
    CHECK_DETAIL((previous & WRITER_BIT) == 0, "ELF unload writers must be serialized");
    unloadWriterImage = imageIdentity;
    unloadWriter = true;
}

void ElfUnloadQuiescence::UnloadScope::Synchronize()
{
    std::unique_lock<std::mutex> lock(DrainMutex());
    DrainCondition().wait(lock, []() {
        return (State().load(std::memory_order_acquire) & READER_MASK) == 0;
    });
    synchronized = true;
}

void ElfUnloadQuiescence::UnloadScope::OpenAdmission()
{
    CHECK_DETAIL(synchronized, "ELF unload admission cannot open before reader drain");
    U64 previous = State().fetch_and(READER_MASK, std::memory_order_release);
    CHECK_DETAIL((previous & WRITER_BIT) != 0, "ELF unload writer bit was not set");
    admissionOpen = true;
}

ElfUnloadQuiescence::UnloadScope::~UnloadScope()
{
    CHECK_DETAIL(synchronized, "ELF unload must drain readers before purge");
    CHECK_DETAIL(admissionOpen, "ELF unload must reopen lookup admission after purge");
    unloadWriter = false;
    unloadWriterImage = 0;
}

ElfUnloadQuiescence::PendingTask::PendingTask(Uptr entryAddress) : entry(entryAddress)
{
    std::shared_lock<std::shared_timed_mutex> admission(TaskAdmissionMutex());
    std::lock_guard<std::mutex> lock(PendingTaskMutex());
    pending = PendingTasks().insert(this).second;
    CHECK_DETAIL(pending, "ELF pending task must be registered exactly once");
}

ElfUnloadQuiescence::PendingTask::~PendingTask()
{
    {
        std::lock_guard<std::mutex> lock(PendingTaskMutex());
        PendingTasks().erase(this);
        pending = false;
    }
    PendingTaskCondition().notify_all();
}

ElfUnloadQuiescence::PendingTask::CompletionScope::CompletionScope(PendingTask& task)
{
    task.MarkCompleted();
}

void ElfUnloadQuiescence::PendingTask::MarkCompleted()
{
    {
        std::lock_guard<std::mutex> lock(PendingTaskMutex());
        CHECK_DETAIL(pending, "ELF task entry must remain registered through managed return");
        size_t erased = PendingTasks().erase(this);
        CHECK_DETAIL(erased == 1, "ELF pending task registry lost an entry");
        pending = false;
    }
    PendingTaskCondition().notify_all();
}

ElfUnloadQuiescence::TaskAdmissionScope::TaskAdmissionScope()
    : admissionLock(TaskAdmissionMutex())
{
}

bool ElfUnloadQuiescence::TaskAdmissionScope::HasPendingForImage(Uptr imageAddress) const
{
    std::lock_guard<std::mutex> lock(PendingTaskMutex());
    for (const PendingTask* task : PendingTasks()) {
        if (IsAddressInImage(task->entry, imageAddress)) {
            return true;
        }
    }
    return false;
}

void ElfUnloadQuiescence::TaskAdmissionScope::WaitUntilNoPendingForImage(Uptr imageAddress) const
{
    std::unique_lock<std::mutex> lock(PendingTaskMutex());
    PendingTaskCondition().wait(lock, [imageAddress]() {
        for (const PendingTask* task : PendingTasks()) {
            if (IsAddressInImage(task->entry, imageAddress)) {
                return false;
            }
        }
        return true;
    });
}

ElfUnloadQuiescence::PurgeAuthorizationScope::PurgeAuthorizationScope(Uptr imageAddress)
    : imageIdentity(ResolveImageIdentity(imageAddress)), previousImageIdentity(purgeAuthorizedImage),
      previousAdmission(purgeAdmission)
{
    CHECK_DETAIL(imageIdentity != 0, "ELF purge authorization image is unavailable for %p",
                 reinterpret_cast<void*>(imageAddress));
    purgeAuthorizedImage = imageIdentity;
}

ElfUnloadQuiescence::PurgeAuthorizationScope::PurgeAuthorizationScope(
    Uptr imageAddress, const TaskAdmissionScope& admission)
    : PurgeAuthorizationScope(imageAddress)
{
    CHECK_DETAIL(purgeAdmission == nullptr, "ELF caller purge protection must not be nested");
    purgeAdmission = &admission;
}

ElfUnloadQuiescence::PurgeAuthorizationScope::~PurgeAuthorizationScope()
{
    CHECK_DETAIL(purgeAuthorizedImage == imageIdentity, "ELF purge authorization changed owner");
    purgeAuthorizedImage = previousImageIdentity;
    purgeAdmission = previousAdmission;
}

void ElfUnloadQuiescence::LinkImage(Uptr imageAddress)
{
    Uptr identity = ResolveImageIdentity(imageAddress);
    CHECK_DETAIL(identity != 0, "ELF load image identity is unavailable for %p",
                 reinterpret_cast<void*>(imageAddress));
    for (auto& slot : linkedImages) {
        Uptr observed = slot.load(std::memory_order_acquire);
        if (observed == identity) {
            return;
        }
        if (observed == 0 && slot.compare_exchange_strong(observed, identity,
                                                          std::memory_order_release,
                                                          std::memory_order_relaxed)) {
            return;
        }
    }
    CHECK_DETAIL(false, "ELF linked-image registry capacity %zu exhausted", MAX_LINKED_IMAGES);
}

void ElfUnloadQuiescence::UnlinkImage(Uptr imageAddress)
{
    Uptr identity = ResolveImageIdentity(imageAddress);
    CHECK_DETAIL(identity != 0, "ELF unload image identity is unavailable for %p",
                 reinterpret_cast<void*>(imageAddress));
    for (auto& slot : linkedImages) {
        if (slot.load(std::memory_order_acquire) == identity) {
            slot.store(0, std::memory_order_release);
            return;
        }
    }
    CHECK_DETAIL(false, "ELF unload image %p was not linked", reinterpret_cast<void*>(identity));
}

bool ElfUnloadQuiescence::IsLinkedAddress(Uptr address)
{
    Uptr identity = ResolveImageIdentity(address);
    if (identity == 0) {
        return false;
    }
    // The dlclose thread is still executing the image's fini callback. Its own
    // frames remain mapped until that callback returns, after this scope ends.
    if (unloadWriter && unloadWriterImage == identity) {
        return true;
    }
    for (auto& slot : linkedImages) {
        if (slot.load(std::memory_order_acquire) == identity) {
            return true;
        }
    }
    return false;
}

bool ElfUnloadQuiescence::IsAddressInImage(Uptr address, Uptr imageAddress)
{
    Uptr identity = ResolveImageIdentity(address);
    return identity != 0 && identity == ResolveImageIdentity(imageAddress);
}

bool ElfUnloadQuiescence::IsPurgeAuthorized(Uptr imageAddress)
{
    return purgeAuthorizedImage != 0 && purgeAuthorizedImage == ResolveImageIdentity(imageAddress);
}

bool ElfUnloadQuiescence::HasCallerPurgeProtection()
{
    return purgeAdmission != nullptr;
}

bool ElfUnloadQuiescence::CallerProtectionHasPendingForImage(Uptr imageAddress)
{
    CHECK_DETAIL(purgeAdmission != nullptr, "ELF caller purge protection is unavailable");
    return purgeAdmission->HasPendingForImage(imageAddress);
}

void ElfUnloadQuiescence::AssertReaderActive()
{
    CHECK_DETAIL(readerDepth != 0, "ELF metadata must be consumed inside a reader scope");
}

#ifdef MRT_TESTABLE_INTERNALS
Uptr ElfUnloadQuiescence::ImageIdentityForTesting(Uptr address)
{
    return ResolveImageIdentity(address);
}

bool ElfUnloadQuiescence::IsImageIdentityLinkedForTesting(Uptr imageIdentity)
{
    for (const auto& slot : linkedImages) {
        if (slot.load(std::memory_order_acquire) == imageIdentity) {
            return true;
        }
    }
    return false;
}

bool ElfUnloadQuiescence::IsUnloadPendingForTesting()
{
    return (State().load(std::memory_order_acquire) & WRITER_BIT) != 0;
}

U64 ElfUnloadQuiescence::GcEntryCountForTesting()
{
    return gcEntryCount.load(std::memory_order_relaxed);
}

void ElfUnloadQuiescence::ResetDirectOrderForTesting()
{
    directPreflightEntered.store(false, std::memory_order_relaxed);
    directEventSequence.store(0, std::memory_order_relaxed);
    directUnlinkEvent.store(0, std::memory_order_relaxed);
    directHandshakeEvent.store(0, std::memory_order_relaxed);
    directPurgeEvent.store(0, std::memory_order_relaxed);
}

void ElfUnloadQuiescence::NoteDirectPreflightForTesting()
{
    directPreflightEntered.store(true, std::memory_order_release);
}

bool ElfUnloadQuiescence::DirectPreflightEnteredForTesting()
{
    return directPreflightEntered.load(std::memory_order_acquire);
}

void ElfUnloadQuiescence::NoteDirectUnlinkForTesting()
{
    directUnlinkEvent.store(directEventSequence.fetch_add(1, std::memory_order_relaxed) + 1,
                            std::memory_order_relaxed);
}

void ElfUnloadQuiescence::NoteDirectHandshakeForTesting()
{
    directHandshakeEvent.store(directEventSequence.fetch_add(1, std::memory_order_relaxed) + 1,
                               std::memory_order_relaxed);
}

void ElfUnloadQuiescence::NoteDirectPurgeForTesting()
{
    directPurgeEvent.store(directEventSequence.fetch_add(1, std::memory_order_relaxed) + 1,
                           std::memory_order_relaxed);
}

bool ElfUnloadQuiescence::DirectOrderValidForTesting()
{
    U64 unlink = directUnlinkEvent.load(std::memory_order_relaxed);
    U64 handshake = directHandshakeEvent.load(std::memory_order_relaxed);
    U64 purge = directPurgeEvent.load(std::memory_order_relaxed);
    return unlink != 0 && unlink < handshake && handshake < purge;
}

void ElfUnloadQuiescence::EnableGcReaderPauseForTesting()
{
    gcReaderPaused.store(false, std::memory_order_relaxed);
    gcReaderReleased.store(false, std::memory_order_relaxed);
    gcReaderPauseEnabled.store(true, std::memory_order_release);
}

bool ElfUnloadQuiescence::GcReaderPausedForTesting()
{
    return gcReaderPaused.load(std::memory_order_acquire);
}

void ElfUnloadQuiescence::ReleaseGcReaderPauseForTesting()
{
    gcReaderReleased.store(true, std::memory_order_release);
}

void ElfUnloadQuiescence::PauseGcReaderForTesting()
{
    if (!gcReaderPauseEnabled.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    gcReaderPaused.store(true, std::memory_order_release);
    while (!gcReaderReleased.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

void ElfUnloadQuiescence::EnablePackageReaderPauseForTesting()
{
    packageReaderPaused.store(false, std::memory_order_relaxed);
    packageReaderReleased.store(false, std::memory_order_relaxed);
    packageReaderPauseEnabled.store(true, std::memory_order_release);
}

bool ElfUnloadQuiescence::PackageReaderPausedForTesting()
{
    return packageReaderPaused.load(std::memory_order_acquire);
}

void ElfUnloadQuiescence::ReleasePackageReaderPauseForTesting()
{
    packageReaderReleased.store(true, std::memory_order_release);
}

void ElfUnloadQuiescence::PausePackageReaderForTesting()
{
    if (!packageReaderPauseEnabled.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    packageReaderPaused.store(true, std::memory_order_release);
    while (!packageReaderReleased.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

extern "C" MRT_EXPORT void* MRT_TestElfUnloadReaderEnter()
{
    return new (std::nothrow) ElfUnloadQuiescence::ReadScope();
}

extern "C" MRT_EXPORT void MRT_TestElfUnloadReaderExit(void* token)
{
    delete static_cast<ElfUnloadQuiescence::ReadScope*>(token);
}

extern "C" MRT_EXPORT bool MRT_TestElfUnloadAddressLinked(Uptr address)
{
    ElfUnloadQuiescence::ReadScope reader;
    return ElfUnloadQuiescence::IsLinkedAddress(address);
}

extern "C" MRT_EXPORT bool MRT_TestElfUnloadPending()
{
    return ElfUnloadQuiescence::IsUnloadPendingForTesting();
}

extern "C" MRT_EXPORT U64 MRT_TestElfUnloadGcEntryCount()
{
    return ElfUnloadQuiescence::GcEntryCountForTesting();
}

extern "C" MRT_EXPORT void MRT_TestElfUnloadResetDirectOrder()
{
    ElfUnloadQuiescence::ResetDirectOrderForTesting();
}

extern "C" MRT_EXPORT bool MRT_TestElfUnloadDirectOrderValid()
{
    return ElfUnloadQuiescence::DirectOrderValidForTesting();
}

extern "C" MRT_EXPORT bool MRT_TestElfUnloadDirectPreflightEntered()
{
    return ElfUnloadQuiescence::DirectPreflightEnteredForTesting();
}

extern "C" MRT_EXPORT void MRT_TestElfUnloadEnableGcReaderPause()
{
    ElfUnloadQuiescence::EnableGcReaderPauseForTesting();
}

extern "C" MRT_EXPORT bool MRT_TestElfUnloadGcReaderPaused()
{
    return ElfUnloadQuiescence::GcReaderPausedForTesting();
}

extern "C" MRT_EXPORT void MRT_TestElfUnloadReleaseGcReaderPause()
{
    ElfUnloadQuiescence::ReleaseGcReaderPauseForTesting();
}

extern "C" MRT_EXPORT void MRT_TestElfUnloadEnablePackageReaderPause()
{
    ElfUnloadQuiescence::EnablePackageReaderPauseForTesting();
}

extern "C" MRT_EXPORT bool MRT_TestElfUnloadPackageReaderPaused()
{
    return ElfUnloadQuiescence::PackageReaderPausedForTesting();
}

extern "C" MRT_EXPORT void MRT_TestElfUnloadReleasePackageReaderPause()
{
    ElfUnloadQuiescence::ReleasePackageReaderPauseForTesting();
}

extern "C" MRT_EXPORT void MRT_TestElfUnloadSynchronize(Uptr imageAddress)
{
    ElfUnloadQuiescence::UnloadScope unload(imageAddress);
    unload.Synchronize();
    unload.OpenAdmission();
}

extern "C" MRT_EXPORT void MRT_TestElfUnloadResetUnrelatedStw()
{
    unrelatedStwHeld.store(false, std::memory_order_relaxed);
    unrelatedStwReleased.store(false, std::memory_order_relaxed);
}

extern "C" MRT_EXPORT bool MRT_TestElfUnloadUnrelatedStwHeld()
{
    return unrelatedStwHeld.load(std::memory_order_acquire);
}

extern "C" MRT_EXPORT void MRT_TestElfUnloadReleaseUnrelatedStw()
{
    unrelatedStwReleased.store(true, std::memory_order_release);
}

extern "C" MRT_EXPORT void MRT_TestElfUnloadHoldUnrelatedStw()
{
    ScopedEnterSaferegion enterSaferegion(false);
    ScopedStopTheWorld stw("ELF unload unrelated STW control", false);
    unrelatedStwHeld.store(true, std::memory_order_release);
    while (!unrelatedStwReleased.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    unrelatedStwHeld.store(false, std::memory_order_release);
}
#endif

} // namespace MapleRuntime
