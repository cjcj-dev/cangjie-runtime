// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Loader/ElfUnloadQuiescence.h"

#include <array>
#include <algorithm>
#include "Base/ImmortalWrapper.h"
#include <climits>
#include <condition_variable>
#include <new>
#include <thread>

#include "Base/Panic.h"
#include "Common/ScopedObjectAccess.h"
#include "Mutator/MutatorManager.h"
#include "RuntimeConfig.h"
#include "schedule.h"
#include "waitqueue.h"

#ifdef _WIN64
#include <windows.h>
#else
#include <dlfcn.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#else
#include <link.h>
#endif
#endif

namespace MapleRuntime {
namespace {
constexpr size_t MAX_LINKED_IMAGES = 4096;
struct AdmissionWaitqueue {
    Waitqueue queue {};
    AdmissionWaitqueue() { CHECK_DETAIL(WaitqueueNew(&queue) == 0, "ELF admission waitqueue creation failed"); }
};
Waitqueue* AdmissionWaiters()
{
    static AdmissionWaitqueue waiters;
    return &waiters.queue;
}
void WakeAdmissionWaiters()
{
    const int rc = WaitqueueWakeAll(AdmissionWaiters(), nullptr, nullptr);
    CHECK_DETAIL(rc == 0 || rc == ERRNO_QUEUE_IS_EMPTY, "ELF admission wake failed: %d", rc);
}
std::array<std::atomic<Uptr>, MAX_LINKED_IMAGES> linkedImages {};
struct ImageDirectory {
    std::mutex mutex;
    std::vector<std::shared_ptr<const ElfUnloadQuiescence::ImageAddressMap>> images;
};
ImageDirectory& ImageMaps()
{
    static ImmortalWrapper<ImageDirectory> maps;
    return *maps;
}
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
std::atomic<bool> directPreflightPauseEnabled { false };
std::atomic<bool> directPreflightPaused { false };
std::atomic<bool> directPreflightReleased { false };
std::atomic<bool> publicPlatformPauseEnabled { false };
std::atomic<bool> publicPlatformPaused { false };
std::atomic<bool> publicPlatformReleased { false };
std::atomic<bool> publicWaitHoldsStw { false };
std::atomic<bool> publicWaitHoldsAdmission { false };
std::atomic<bool> forcePublicHoldAcrossPlatform { false };
std::atomic<bool> skipImageClosing { false };
std::atomic<bool> failNextPlatformUnload { false };
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

std::mutex& ElfUnloadQuiescence::ClosingMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::unordered_set<Uptr>& ElfUnloadQuiescence::ClosingIdentities()
{
    static std::unordered_set<Uptr> identities;
    return identities;
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

bool ElfUnloadQuiescence::ImageAddressMap::Contains(Uptr address, bool codeOnly) const
{
    for (const auto& range : ranges) {
        if ((!codeOnly || range.executable) && address >= range.start && address - range.start < range.size) {
            return true;
        }
    }
    return false;
}

std::shared_ptr<const ElfUnloadQuiescence::ImageAddressMap> ElfUnloadQuiescence::RegisteredImage(Uptr metadata)
{
    auto& maps = ImageMaps();
    std::lock_guard<std::mutex> lock(maps.mutex);
    for (const auto& image : maps.images) {
        if (image->metadata == metadata) { return image; }
    }
    return nullptr;
}

std::shared_ptr<const ElfUnloadQuiescence::ImageAddressMap> ElfUnloadQuiescence::RegisteredImageForAddress(Uptr address)
{
    auto& maps = ImageMaps();
    std::lock_guard<std::mutex> lock(maps.mutex);
    for (const auto& image : maps.images) {
        if (image->Contains(address)) { return image; }
    }
    return nullptr;
}

Uptr ElfUnloadQuiescence::RegisteredIdentity(Uptr metadata)
{
    const auto image = RegisteredImage(metadata);
    CHECK_DETAIL(image != nullptr, "ELF image metadata was not registered: %p", reinterpret_cast<void*>(metadata));
    return image->identity;
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
    : writerLock(WriterMutex()), imageIdentity(RegisteredIdentity(imageAddress))
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

bool ElfUnloadQuiescence::SharedTaskAdmissionScope::TryAcquire(void* scope)
{
    return static_cast<SharedTaskAdmissionScope*>(scope)->admissionLock.try_lock();
}

ElfUnloadQuiescence::SharedTaskAdmissionScope::SharedTaskAdmissionScope()
    : admissionLock(TaskAdmissionMutex(), std::defer_lock)
{
    ScopedEnterSaferegion safe(false);
    if (CJThreadGetHandle() == nullptr) {
        admissionLock.lock();
        return;
    }
    while (!admissionLock.owns_lock()) {
        if (admissionLock.try_lock()) { break; }
        // The callback retries under the queue lock. A release either precedes
        // this retry or wakes the node after it has atomically entered the queue.
        int rc = WaitqueuePark(AdmissionWaiters(), LLONG_MAX, TryAcquire, this, false);
        CHECK_DETAIL(rc == 0 || rc == ERRNO_CALLBACK_RETURN_TRUE, "ELF admission park failed: %d", rc);
    }
}

ElfUnloadQuiescence::PendingTask::PendingTask(Uptr entryAddress)
    : PendingTask(entryAddress, SharedTaskAdmissionScope())
{
}

ElfUnloadQuiescence::PendingTask::PendingTask(Uptr entryAddress, const SharedTaskAdmissionScope& admission)
    : entry(entryAddress)
{
    CHECK_DETAIL(admission.admissionLock.owns_lock(), "ELF task registration requires shared admission");
    image = RegisteredImageForAddress(entryAddress);
    CHECK_DETAIL(image == nullptr || !IsImageClosing(image->metadata),
                 "ELF task registration cannot cross a committed image close");
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
    WakeAdmissionWaiters();
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
    WakeAdmissionWaiters();
}

ElfUnloadQuiescence::TaskAdmissionScope::TaskAdmissionScope()
    : admissionLock(TaskAdmissionMutex())
{
}

ElfUnloadQuiescence::TaskAdmissionScope::~TaskAdmissionScope()
{
    admissionLock.unlock();
    WakeAdmissionWaiters();
}

bool ElfUnloadQuiescence::TaskAdmissionScope::HasPendingForImage(Uptr imageAddress) const
{
    const Uptr identity = RegisteredIdentity(imageAddress);
    std::lock_guard<std::mutex> lock(PendingTaskMutex());
    for (const PendingTask* task : PendingTasks()) {
        if (task->image != nullptr && task->image->identity == identity) {
            return true;
        }
    }
    return false;
}

void ElfUnloadQuiescence::TaskAdmissionScope::WaitUntilNoPendingForImage(Uptr imageAddress) const
{
    WaitForPendingTasks(imageAddress);
}

bool ElfUnloadQuiescence::BeginImageClosing(Uptr imageAddress)
{
#ifdef MRT_TESTABLE_INTERNALS
    if (skipImageClosing.load(std::memory_order_acquire)) {
        return true;
    }
#endif
    const Uptr identity = RegisteredIdentity(imageAddress);
    std::lock_guard<std::mutex> lock(ClosingMutex());
    return ClosingIdentities().insert(identity).second;
}

void ElfUnloadQuiescence::AbortImageClosing(Uptr imageAddress)
{
    const auto image = RegisteredImage(imageAddress);
    if (image == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(ClosingMutex());
    ClosingIdentities().erase(image->identity);
}

void ElfUnloadQuiescence::CommitImageClosing(Uptr imageAddress)
{
    Uptr identity = 0;
    if (const auto image = RegisteredImage(imageAddress)) {
        identity = image->identity;
    } else {
        identity = ResolveImageIdentity(imageAddress);
    }
    std::lock_guard<std::mutex> lock(ClosingMutex());
    if (identity != 0) {
        ClosingIdentities().erase(identity);
    }
}

bool ElfUnloadQuiescence::IsImageClosing(Uptr imageAddress)
{
    const auto image = RegisteredImage(imageAddress);
    const Uptr identity = image != nullptr ? image->identity : ResolveImageIdentity(imageAddress);
    if (identity == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(ClosingMutex());
    return ClosingIdentities().count(identity) != 0;
}

void ElfUnloadQuiescence::WaitForPendingTasks(Uptr imageAddress)
{
    Uptr identity = RegisteredIdentity(imageAddress);
    const auto empty = [](Uptr identity) {
        for (const PendingTask* task : PendingTasks()) {
            if (task->image != nullptr && task->image->identity == identity) { return false; }
        }
        return true;
    };
    if (CJThreadGetHandle() == nullptr) {
        std::unique_lock<std::mutex> lock(PendingTaskMutex());
        PendingTaskCondition().wait(lock, [&]() { return empty(identity); });
        return;
    }
    ScopedEnterSaferegion safe(false);
    const auto finished = [](void* address) {
        std::lock_guard<std::mutex> lock(PendingTaskMutex());
        for (const PendingTask* task : PendingTasks()) {
            if (task->image != nullptr && task->image->identity == *static_cast<const Uptr*>(address)) { return false; }
        }
        return true;
    };
    while (!finished(&identity)) {
        const int rc = WaitqueuePark(AdmissionWaiters(), LLONG_MAX, finished, &identity, false);
        CHECK_DETAIL(rc == 0 || rc == ERRNO_CALLBACK_RETURN_TRUE, "ELF pending task park failed: %d", rc);
    }
}

ElfUnloadQuiescence::PurgeAuthorizationScope::PurgeAuthorizationScope(Uptr imageAddress)
    : imageIdentity(RegisteredIdentity(imageAddress)), previousImageIdentity(purgeAuthorizedImage),
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

std::shared_ptr<const ElfUnloadQuiescence::ImageAddressMap> ElfUnloadQuiescence::LinkImage(Uptr imageAddress)
{
    // Capture once while the image is being registered, before runtime locks.
    // Dependency Begin and pending checks must not call into the platform
    // loader while dlclose is waiting in an image's fini callback.
    auto image = std::make_shared<ImageAddressMap>();
    image->metadata = imageAddress;
    image->identity = ResolveImageIdentity(imageAddress);
    CHECK_DETAIL(image->identity != 0, "ELF load image identity is unavailable");
    CHECK_DETAIL(!IsImageClosing(imageAddress), "ELF load cannot reopen a closing image generation");
    static std::atomic<U64> nextGeneration { 1 };
    image->generation = nextGeneration.fetch_add(1, std::memory_order_relaxed);
#if defined(_WIN64)
    Uptr cursor = image->identity;
    MEMORY_BASIC_INFORMATION info {};
    constexpr DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    while (VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info)) != 0 &&
           reinterpret_cast<Uptr>(info.AllocationBase) == image->identity) {
        if (info.State == MEM_COMMIT) {
            image->ranges.push_back({ reinterpret_cast<Uptr>(info.BaseAddress), info.RegionSize,
                                      (info.Protect & executable) != 0 });
        }
        cursor = reinterpret_cast<Uptr>(info.BaseAddress) + info.RegionSize;
    }
#elif defined(__APPLE__)
    for (uint32_t i = 0; i != _dyld_image_count(); ++i) {
        const auto* header = _dyld_get_image_header(i);
        if (reinterpret_cast<Uptr>(header) != image->identity) { continue; }
        const auto slide = _dyld_get_image_vmaddr_slide(i);
        auto* command = reinterpret_cast<const load_command*>(reinterpret_cast<const mach_header_64*>(header) + 1);
        for (uint32_t n = 0; n != header->ncmds; ++n) {
            if (command->cmd == LC_SEGMENT_64) {
                auto* segment = reinterpret_cast<const segment_command_64*>(command);
                if (segment->initprot != 0 && segment->vmsize != 0) {
                    image->ranges.push_back({ static_cast<Uptr>(segment->vmaddr + slide),
                        static_cast<size_t>(segment->vmsize), (segment->initprot & VM_PROT_EXECUTE) != 0 });
                }
            }
            command = reinterpret_cast<const load_command*>(reinterpret_cast<const char*>(command) + command->cmdsize);
        }
        break;
    }
#else
    dl_iterate_phdr([](dl_phdr_info* loaded, size_t, void* argument) {
        auto& map = *static_cast<ImageAddressMap*>(argument);
        bool containsMetadata = false;
        for (size_t i = 0; i != loaded->dlpi_phnum; ++i) {
            const auto& segment = loaded->dlpi_phdr[i];
            const Uptr start = loaded->dlpi_addr + segment.p_vaddr;
            containsMetadata |= segment.p_type == PT_LOAD && map.metadata >= start &&
                map.metadata - start < segment.p_memsz;
        }
        if (!containsMetadata) { return 0; }
        for (size_t i = 0; i != loaded->dlpi_phnum; ++i) {
            const auto& segment = loaded->dlpi_phdr[i];
            if (segment.p_type == PT_LOAD && segment.p_memsz != 0) {
                map.ranges.push_back({ static_cast<Uptr>(loaded->dlpi_addr + segment.p_vaddr),
                    static_cast<size_t>(segment.p_memsz), (segment.p_flags & PF_X) != 0 });
            }
        }
        return 1;
    }, image.get());
#endif
    CHECK_DETAIL(image->Contains(imageAddress), "registered image must contain its metadata");
    auto& maps = ImageMaps();
    std::lock_guard<std::mutex> lock(maps.mutex);
    maps.images.push_back(image);
    for (auto& slot : linkedImages) {
        Uptr observed = slot.load(std::memory_order_acquire);
        if (observed == image->identity) { return image; }
        if (observed == 0 && slot.compare_exchange_strong(observed, image->identity,
                                                          std::memory_order_release,
                                                          std::memory_order_relaxed)) { return image; }
    }
    CHECK_DETAIL(false, "ELF linked-image registry capacity %zu exhausted", MAX_LINKED_IMAGES);
    return image;
}

void ElfUnloadQuiescence::UnlinkImage(Uptr imageAddress)
{
    auto& maps = ImageMaps();
    std::lock_guard<std::mutex> lock(maps.mutex);
    auto found = std::find_if(maps.images.begin(), maps.images.end(), [imageAddress](const auto& image) {
        return image->metadata == imageAddress;
    });
    CHECK_DETAIL(found != maps.images.end(), "ELF unload image was not linked");
    const Uptr identity = (*found)->identity;
    maps.images.erase(found);
    for (const auto& image : maps.images) {
        if (image->identity == identity) { return; }
    }
    for (auto& slot : linkedImages) {
        if (slot.load(std::memory_order_acquire) == identity) {
            slot.store(0, std::memory_order_release);
            return;
        }
    }
    CHECK_DETAIL(false, "ELF unload image identity was not linked");
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
    const auto image = RegisteredImage(imageAddress);
    return image != nullptr && image->Contains(address);
}

bool ElfUnloadQuiescence::IsPurgeAuthorized(Uptr imageAddress)
{
    return purgeAuthorizedImage != 0 && purgeAuthorizedImage == RegisteredIdentity(imageAddress);
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

void ElfUnloadQuiescence::EnableDirectPreflightPauseForTesting()
{
    directPreflightPaused.store(false, std::memory_order_relaxed);
    directPreflightReleased.store(false, std::memory_order_relaxed);
    directPreflightPauseEnabled.store(true, std::memory_order_release);
}

bool ElfUnloadQuiescence::DirectPreflightPausedForTesting()
{
    return directPreflightPaused.load(std::memory_order_acquire);
}

void ElfUnloadQuiescence::ReleaseDirectPreflightPauseForTesting()
{
    directPreflightReleased.store(true, std::memory_order_release);
}

void ElfUnloadQuiescence::PauseDirectPreflightForTesting()
{
    if (!directPreflightPauseEnabled.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    directPreflightPaused.store(true, std::memory_order_release);
    while (!directPreflightReleased.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

void ElfUnloadQuiescence::EnablePublicPlatformPauseForTesting()
{
    publicPlatformPaused.store(false, std::memory_order_relaxed);
    publicPlatformReleased.store(false, std::memory_order_relaxed);
    publicPlatformPauseEnabled.store(true, std::memory_order_release);
}

bool ElfUnloadQuiescence::PublicPlatformPausedForTesting()
{
    return publicPlatformPaused.load(std::memory_order_acquire);
}

void ElfUnloadQuiescence::ReleasePublicPlatformPauseForTesting()
{
    publicPlatformReleased.store(true, std::memory_order_release);
}

void ElfUnloadQuiescence::PausePublicPlatformForTesting()
{
    if (!publicPlatformPauseEnabled.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    publicPlatformPaused.store(true, std::memory_order_release);
    while (!publicPlatformReleased.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

void ElfUnloadQuiescence::NotePublicPlatformWaitForTesting(bool holdsStw, bool holdsAdmission)
{
    publicWaitHoldsStw.store(holdsStw, std::memory_order_release);
    publicWaitHoldsAdmission.store(holdsAdmission, std::memory_order_release);
}

bool ElfUnloadQuiescence::PublicPlatformWaitHoldsStwForTesting()
{
    return publicWaitHoldsStw.load(std::memory_order_acquire);
}

bool ElfUnloadQuiescence::PublicPlatformWaitHoldsAdmissionForTesting()
{
    return publicWaitHoldsAdmission.load(std::memory_order_acquire);
}

void ElfUnloadQuiescence::ForcePublicHoldAcrossPlatformForTesting(bool enable)
{
    forcePublicHoldAcrossPlatform.store(enable, std::memory_order_release);
}

bool ElfUnloadQuiescence::PublicHoldAcrossPlatformForTesting()
{
    return forcePublicHoldAcrossPlatform.load(std::memory_order_acquire);
}

void ElfUnloadQuiescence::SkipImageClosingForTesting(bool enable)
{
    skipImageClosing.store(enable, std::memory_order_release);
}

bool ElfUnloadQuiescence::ImageClosingSkippedForTesting()
{
    return skipImageClosing.load(std::memory_order_acquire);
}

void ElfUnloadQuiescence::FailNextPlatformUnloadForTesting(bool enable)
{
    failNextPlatformUnload.store(enable, std::memory_order_release);
}

bool ElfUnloadQuiescence::ConsumeFailedPlatformUnloadForTesting()
{
    return failNextPlatformUnload.exchange(false, std::memory_order_acq_rel);
}
#endif

} // namespace MapleRuntime
