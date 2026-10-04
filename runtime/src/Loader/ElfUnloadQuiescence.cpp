// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Loader/ElfUnloadQuiescence.h"

#include <map>
#include <algorithm>
#include "Base/ImmortalWrapper.h"
#include <climits>
#include <cstring>
#include <condition_variable>
#include <new>
#include <thread>

#include "Base/Panic.h"
#include "Loader/BinaryFile/CjFile/CjFileMeta.h"
#include "ObjectModel/MFuncdesc.h"
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
using ImageMap = ElfUnloadQuiescence::ImageAddressMap;
struct ImageInterval {
    Uptr start;
    Uptr last;
    std::shared_ptr<const ImageMap> image;
    bool executable;
};
std::shared_ptr<const ImageMap> FindImageInterval(const std::vector<ImageInterval>& intervals,
                                               Uptr address, bool codeOnly)
{
    auto found = std::upper_bound(intervals.begin(), intervals.end(), address,
        [](Uptr pc, const ImageInterval& interval) { return pc < interval.start; });
    if (found == intervals.begin()) { return nullptr; }
    --found;
    return address <= found->last && (!codeOnly || found->executable) ? found->image : nullptr;
}
struct ImageDirectory {
    std::mutex mutex;
    std::vector<std::shared_ptr<const ImageMap>> images;
    std::vector<ImageInterval> intervals;

    // CodeCache::find_blob (codeCache.cpp:750): publish address ownership
    // before metadata lookup. Split overlapping LOAD segments and duplicate
    // metadata registrations into disjoint intervals at publication time.
    void Rebuild()
    {
        struct Event { Uptr address; size_t index; bool begin; };
        std::vector<ImageInterval> ranges;
        std::vector<Event> events;
        for (const auto& image : images) {
            for (const auto& range : image->ranges) {
                if (range.size == 0) { continue; }
                const Uptr last = range.start + std::min<Uptr>(range.size - 1, ~Uptr(0) - range.start);
                const size_t index = ranges.size();
                ranges.push_back({range.start, last, image, range.executable});
                events.push_back({range.start, index, true});
                if (last != ~Uptr(0)) { events.push_back({last + 1, index, false}); }
            }
        }
        std::sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
            return a.address < b.address;
        });
        intervals.clear();
        std::map<size_t, const ImageInterval*> active;
        size_t executable = 0;
        for (size_t i = 0; i < events.size();) {
            const Uptr start = events[i].address;
            do {
                const auto& event = events[i++];
                const auto& range = ranges[event.index];
                if (event.begin) { active.emplace(event.index, &range); executable += range.executable; }
                else { active.erase(event.index); executable -= range.executable; }
            } while (i < events.size() && events[i].address == start);
            if (!active.empty()) {
                const Uptr last = i == events.size() ? ~Uptr(0) : events[i].address - 1;
                intervals.push_back({start, last, active.begin()->second->image, executable != 0});
            }
        }
    }

    std::shared_ptr<const ImageMap> Find(Uptr address, bool codeOnly = false) const
    {
        return FindImageInterval(intervals, address, codeOnly);
    }
};
ImageDirectory& ImageMaps()
{
    static ImmortalWrapper<ImageDirectory> maps;
    return *maps;
}
thread_local U32 readerDepth = 0;
thread_local bool unloadWriter = false;
thread_local std::vector<ImageInterval> unloadWriterIntervals;
thread_local Uptr purgeAuthorizedImage = 0;
thread_local const ElfUnloadQuiescence::TaskAdmissionScope* purgeAdmission = nullptr;
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
    auto found = std::upper_bound(ranges.begin(), ranges.end(), address,
        [](Uptr pc, const Range& range) { return pc < range.start; });
    if (found == ranges.begin()) { return false; }
    --found;
    return (!codeOnly || found->executable) && address - found->start < found->size;
}

bool ElfUnloadQuiescence::ImageAddressMap::ContainsFunctionDescriptor(Uptr descriptor) const
{
    // Existing AOT ABI: the prefix/frame slot selects a record in the registered
    // FUNC_DESC_TABLE. CodeCache::find_blob (codeCache.cpp:750-759) establishes
    // ownership before nmethod metadata is consumed; mapped bytes alone do not.
    const auto readable = [this](Uptr first, size_t size) {
        if (size == 0 || size - 1 > UINTPTR_MAX - first) { return false; }
        for (size_t i = 0; i != size; ++i) {
            if (!Contains(first + i)) { return false; }
        }
        return true;
    };
    constexpr size_t tableHeaderSize = offsetof(CJFileHeader, tables) + sizeof(TableDesc);
    if (!readable(metadata, tableHeaderSize)) { return false; }
    const auto* header = reinterpret_cast<const CJFileHeader*>(metadata);
    if (header->magic != 0x12345678) { return false; }
    const auto& table = header->tables[FUNC_DESC_TABLE];
    Uptr first;
    U32 size;
#if defined(_WIN64) || defined(__APPLE__)
    if (!readable(reinterpret_cast<Uptr>(table.tableAddr), sizeof(*table.tableAddr)) ||
        !readable(reinterpret_cast<Uptr>(table.tableSize), sizeof(*table.tableSize))) {
        return false;
    }
    first = *table.tableAddr;
    size = *table.tableSize;
#else
    if (table.tableOffset > UINTPTR_MAX - metadata) { return false; }
    first = metadata + table.tableOffset;
    size = table.tableSize;
#endif
    constexpr size_t stride = sizeof(MFuncDesc);
#ifdef __APPLE__
    static_assert(stride == 56, "AOT qualification MachO function descriptor ABI");
#else
    static_assert(stride == 48, "AOT qualification ELF/PE function descriptor ABI");
#endif
    if (size < stride || size % stride != 0 || size - 1 > UINTPTR_MAX - first || descriptor < first) { return false; }
    const Uptr offset = descriptor - first;
    return offset % stride == 0 && offset <= size - stride && readable(descriptor, stride);
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

std::shared_ptr<const ElfUnloadQuiescence::ImageAddressMap> ElfUnloadQuiescence::RegisteredImageForAddress(Uptr address, bool codeOnly)
{
    auto& maps = ImageMaps();
    std::lock_guard<std::mutex> lock(maps.mutex);
    // A writer owns the exact retiring registration even if another header
    // of the same DSO still publishes overlapping addresses.
    if (unloadWriter) {
        const auto image = FindImageInterval(unloadWriterIntervals, address, codeOnly);
        if (image != nullptr) { return image; }
    }
    return maps.Find(address, codeOnly);
}

#ifdef __APPLE__
Uptr ElfUnloadQuiescence::ImageAddressMap::FindFunctionDescriptor(Uptr startPC) const
{
    const auto found = std::lower_bound(functions.begin(), functions.end(), startPC,
        [](const Function& function, Uptr pc) { return function.startPC < pc; });
    return found != functions.end() && found->startPC == startPC ? found->descriptor : 0;
}

Uptr ElfUnloadQuiescence::FindFunctionDescriptor(Uptr startPC)
{
    // CodeCache::find_blob selects the owning heap, then delegates its lookup.
    AssertReaderActive();
    const auto image = RegisteredImageForAddress(startPC, true);
    if (image == nullptr) { return 0; }
    const Uptr descriptor = image->FindFunctionDescriptor(startPC);
    return image->ContainsFunctionDescriptor(descriptor) ? descriptor : 0;
}
#endif

Uptr ElfUnloadQuiescence::RegisteredIdentity(Uptr metadata)
{
    const auto image = RegisteredImage(metadata);
    CHECK_DETAIL(image != nullptr, "ELF image metadata was not registered: %p", reinterpret_cast<void*>(metadata));
    return image->identity;
}

ElfUnloadQuiescence::ReadScope::ReadScope(ReaderKind kind)
{
    (void)kind;
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
    const auto image = RegisteredImage(imageAddress);
    CHECK_DETAIL(image != nullptr && image->identity == imageIdentity,
                 "ELF unload must retain its exact registered image");
    for (const auto& range : image->ranges) {
        if (range.size == 0) { continue; }
        const Uptr last = range.start + std::min<Uptr>(range.size - 1, ~Uptr(0) - range.start);
        unloadWriterIntervals.push_back({range.start, last, image, range.executable});
    }
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
    unloadWriterIntervals.clear();
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
                const auto* sections = reinterpret_cast<const section_64*>(segment + 1);
                for (uint32_t j = 0; j != segment->nsects; ++j) {
                    const auto& section = sections[j];
                    if (std::strncmp(section.segname, "__CJ_METADATA", sizeof(section.segname)) != 0 ||
                        std::strncmp(section.sectname, "__cjfuncmap", sizeof(section.sectname)) != 0) {
                        continue;
                    }
                    using Function = ImageAddressMap::Function;
                    static_assert(sizeof(Function) == 16, "MachO function map ABI");
                    const auto* records = reinterpret_cast<const Function*>(section.addr + slide);
                    image->functions.assign(records, records + section.size / sizeof(Function));
                }
            }
            command = reinterpret_cast<const load_command*>(reinterpret_cast<const char*>(command) + command->cmdsize);
        }
        // CodeCache::find_blob (codeCache.cpp:750-759): publish a PC index,
        // never recover return-site metadata from an already removed frame.
        std::sort(image->functions.begin(), image->functions.end(),
                  [](const ImageAddressMap::Function& a, const ImageAddressMap::Function& b) {
                      return a.startPC < b.startPC;
                  });
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
    // Normalize LOAD overlaps once; both per-image membership and the public
    // directory use the same disjoint interval shape and binary lookup.
    ImageDirectory normalized;
    normalized.images.push_back(image);
    normalized.Rebuild();
    image->ranges.clear();
    for (const auto& interval : normalized.intervals) {
        image->ranges.push_back({interval.start, interval.last - interval.start + 1, interval.executable});
    }
    CHECK_DETAIL(image->Contains(imageAddress), "registered image must contain its metadata");
    auto& maps = ImageMaps();
    std::lock_guard<std::mutex> lock(maps.mutex);
    maps.images.push_back(image);
    maps.Rebuild();
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
    maps.images.erase(found);
    maps.Rebuild();
}

bool ElfUnloadQuiescence::IsLinkedAddress(Uptr address, bool codeOnly)
{
    return RegisteredImageForAddress(address, codeOnly) != nullptr;
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


} // namespace MapleRuntime
