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
struct ImageSnapshot {
    std::vector<std::shared_ptr<const ImageMap>> images;
    std::vector<ImageInterval> intervals;
};
struct ImageDirectory {
    std::mutex mutex;
    std::vector<std::shared_ptr<const ImageMap>> images;
    std::vector<ImageInterval> intervals;

    std::atomic<const ImageSnapshot*> snapshot { nullptr };
    std::unique_ptr<ImageSnapshot> published;
    std::vector<std::unique_ptr<ImageSnapshot>> retired;

    void Publish()
    {
        auto next = std::unique_ptr<ImageSnapshot>(new ImageSnapshot {images, intervals});
        if (published != nullptr) { retired.push_back(std::move(published)); }
        published = std::move(next);
        snapshot.store(published.get(), std::memory_order_release);
    }

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
thread_local std::unique_ptr<ImageSnapshot> unloadWriterSnapshot;
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

bool ElfUnloadQuiescence::ImageAddressMap::Covers(Uptr address, size_t size, bool codeOnly) const
{
    if (size == 0) { return true; }
    if (size - 1 > UINTPTR_MAX - address) { return false; }
    // Check the complete union, including internal gaps, without reading it.
    for (const auto& range : ranges) {
        if (range.start > address) { return false; }
        if (address - range.start >= range.size || (codeOnly && !range.executable)) { continue; }
        const size_t available = range.size - (address - range.start);
        if (size <= available) { return true; }
        size -= available;
        if (available > UINTPTR_MAX - address) { return false; }
        address += available;
    }
    return false;
}

namespace {
template<class T> T ReadMetadata(Uptr address)
{
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}
Uptr RelativeMetadataAddress(Uptr field, I64 offset)
{
    if (offset >= 0) {
        CHECK_DETAIL(static_cast<U64>(offset) <= UINTPTR_MAX - field, "metadata relative address overflow");
        return field + static_cast<Uptr>(offset);
    }
    const U64 magnitude = static_cast<U64>(-(offset + 1)) + 1;
    CHECK_DETAIL(magnitude <= field, "metadata relative address underflow");
    return field - static_cast<Uptr>(magnitude);
}
struct MetadataTable { Uptr start; U32 size; };
MetadataTable ReadMetadataTable(const ImageMap& image, const CJFileHeader& header, CFileTable index)
{
    const auto& table = header.tables[index];
#if defined(_WIN64) || defined(__APPLE__)
    CHECK_DETAIL(image.Covers(reinterpret_cast<Uptr>(table.tableAddr), sizeof(U64)) &&
                 image.Covers(reinterpret_cast<Uptr>(table.tableSize), sizeof(U32)),
                 "metadata table indirection is outside owner");
    const MetadataTable result {ReadMetadata<U64>(reinterpret_cast<Uptr>(table.tableAddr)),
                               ReadMetadata<U32>(reinterpret_cast<Uptr>(table.tableSize))};
#else
    CHECK_DETAIL(table.tableOffset <= UINTPTR_MAX - image.metadata, "metadata table address overflow");
    const MetadataTable result {image.metadata + table.tableOffset, table.tableSize};
#endif
    CHECK_DETAIL(image.Covers(result.start, result.size), "metadata table is outside readable owner");
    return result;
}
void CheckMetadataHeader(const ImageMap& image)
{
    CHECK_DETAIL(image.Covers(image.metadata, sizeof(CJFileHeader)), "CJ header is outside readable owner");
    const auto header = ReadMetadata<CJFileHeader>(image.metadata);
    CHECK_DETAIL(header.magic == 0x12345678, "invalid CJ metadata magic");
    CHECK_DETAIL(header.version == 0x80000001, "old AOT qualification ABI: rebuild CJ objects and std");
#if defined(_WIN64) || defined(__APPLE__)
    CHECK_DETAIL(image.Covers(reinterpret_cast<Uptr>(header.cJFileSize), sizeof(U32)), "CJ size indirection outside owner");
    const U32 fileSize = ReadMetadata<U32>(reinterpret_cast<Uptr>(header.cJFileSize));
#else
    const U32 fileSize = header.cJFileSize;
#endif
    CHECK_DETAIL(fileSize <= UINTPTR_MAX - image.metadata, "CJ metadata end overflow");
    for (U32 i = 0; i != C_FILE_MAX; ++i) { (void)ReadMetadataTable(image, header, static_cast<CFileTable>(i)); }
}
} // namespace

void ElfUnloadQuiescence::ValidateFileHeader(Uptr metadata)
{
    const auto image = CaptureImage(metadata);
    CheckMetadataHeader(*image);
}

const char* ElfUnloadQuiescence::ValidatedSDKVersion(Uptr metadata)
{
    const auto image = CaptureImage(metadata);
    CheckMetadataHeader(*image);
    const auto header = ReadMetadata<CJFileHeader>(metadata);
#if defined(_WIN64) || defined(__APPLE__)
    CHECK_DETAIL(image->Covers(reinterpret_cast<Uptr>(header.cJFileSize), sizeof(U32)), "CJ size indirection outside owner");
    CHECK_DETAIL(image->Covers(reinterpret_cast<Uptr>(header.cJSDKVersionPtr), sizeof(U64)), "CJ SDK indirection outside owner");
    const Uptr versionSlot = ReadMetadata<U64>(reinterpret_cast<Uptr>(header.cJSDKVersionPtr));
#else
    CHECK_DETAIL(header.cJSDKVersionOffset <= UINTPTR_MAX - metadata, "CJ SDK offset overflow");
    const Uptr versionSlot = metadata + header.cJSDKVersionOffset;
#endif
    CHECK_DETAIL(image->Covers(versionSlot, sizeof(Uptr)), "CJ SDK version pointer outside owner");
    const Uptr version = ReadMetadata<Uptr>(versionSlot);
    Uptr end = version;
    for (;;) {
        CHECK_DETAIL(image->Covers(end, 1), "CJ SDK string outside owner");
        if (ReadMetadata<U8>(end) == 0) { break; }
        CHECK_DETAIL(end != UINTPTR_MAX, "CJ SDK string overflow");
        ++end;
    }
    return reinterpret_cast<const char*>(version);
}

void ElfUnloadQuiescence::ImageAddressMap::ValidateMetadata()
{
    CheckMetadataHeader(*this);
    const auto header = ReadMetadata<CJFileHeader>(metadata);
    const auto descriptors = ReadMetadataTable(*this, header, FUNC_DESC_TABLE);
    const auto maps = ReadMetadataTable(*this, header, STACK_MAP_TABLE);
    CHECK_DETAIL(descriptors.start % alignof(MFuncDesc) == 0 && descriptors.size % sizeof(MFuncDesc) == 0, "invalid AOT descriptor stride");
    descriptorStart = descriptors.start;
    descriptorBytes = descriptors.size;
    for (size_t offset = 0; offset < descriptors.size; offset += sizeof(MFuncDesc)) {
        const Uptr descriptor = descriptors.start + offset;
#ifdef __APPLE__
        constexpr size_t entryOffset = 40, qualificationOffset = 48, tagOffset = 52;
        const I64 entryRel = ReadMetadata<I64>(descriptor + entryOffset);
#else
        constexpr size_t entryOffset = 32, qualificationOffset = 36, tagOffset = 40;
        const I64 entryRel = ReadMetadata<I32>(descriptor + entryOffset);
        CHECK_DETAIL(ReadMetadata<U32>(descriptor + 44) == 0, "invalid AOT reserved bits");
#endif
        CHECK_DETAIL(ReadMetadata<U32>(descriptor + tagOffset) == MFuncDesc::AOT_QUALIFICATION_TAG,
                     "invalid AOT descriptor tag: rebuild CJ objects and std");
        const I32 qualificationRel = ReadMetadata<I32>(descriptor + qualificationOffset);
        CHECK_DETAIL(entryRel != 0 && qualificationRel != 0, "missing AOT entry or qualification");
        const Uptr entry = RelativeMetadataAddress(descriptor + entryOffset, entryRel);
        const Uptr qualification = RelativeMetadataAddress(descriptor + qualificationOffset, qualificationRel);
        const U32 codeSize = ReadMetadata<U32>(descriptor + 4);
        CHECK_DETAIL(codeSize != 0 && codeSize <= UINTPTR_MAX - entry && Covers(entry, codeSize, true),
                     "AOT function extent is outside executable owner");
#ifndef __APPLE__
        CHECK_DETAIL(entry >= 4 && Covers(entry - 4, 4) &&
                     RelativeMetadataAddress(entry - 4, ReadMetadata<I32>(entry - 4)) == descriptor,
                     "AOT function prefix disagrees with descriptor");
#endif
        const auto inMaps = [&maps](Uptr address, size_t size) {
            return address >= maps.start && address - maps.start <= maps.size &&
                   size <= maps.size - (address - maps.start);
        };
        CHECK_DETAIL(qualification % 4 == 0 && inMaps(qualification, 16), "AOT qualification header outside map table");
        const U32 tag = ReadMetadata<U32>(qualification);
        const U32 bytes = ReadMetadata<U32>(qualification + 4);
        const U32 transitions = ReadMetadata<U32>(qualification + 8);
        const U32 sites = ReadMetadata<U32>(qualification + 12);
        CHECK_DETAIL(tag == MFuncDesc::AOT_QUALIFICATION_TAG && transitions != 0 &&
                     U64 {16} + U64 {8} * (U64 {transitions} + sites) == bytes && inMaps(qualification, bytes),
                     "invalid AOT qualification payload");
        const I32 mapRel = ReadMetadata<I32>(descriptor);
        if (mapRel != 0) {
            const Uptr map = RelativeMetadataAddress(descriptor, mapRel);
            CHECK_DETAIL(inMaps(map, 1) && map < qualification, "AOT map must precede its qualification");
        }
        Function function {entry, descriptor, codeSize, qualification, {}, {}};
        for (U32 i = 0; i != transitions; ++i) {
            const Uptr row = qualification + 16 + size_t {8} * i;
            const Transition transition {ReadMetadata<U32>(row), ReadMetadata<U32>(row + 4)};
            CHECK_DETAIL(transition.offset < codeSize && (transition.bits & ~U32 {3}) == 0 &&
                         (i == 0 ? transition.offset == 0 : transition.offset > function.transitions.back().offset),
                         "invalid AOT layout transition");
            function.transitions.push_back(transition);
        }
        for (U32 i = 0; i != sites; ++i) {
            const Uptr row = qualification + 16 + size_t {8} * (size_t {transitions} + i);
            const Site site {ReadMetadata<U32>(row), ReadMetadata<U16>(row + 4), ReadMetadata<U16>(row + 6)};
            CHECK_DETAIL(site.offset <= codeSize && site.kind >= 1 && site.kind <= 3 && (site.bits & ~U16 {3}) == 0,
                         "invalid AOT saved site");
            if (i != 0) {
                const auto& previous = function.sites.back();
                CHECK_DETAIL(site.offset > previous.offset || (site.offset == previous.offset && site.kind > previous.kind),
                             "AOT saved sites must be strictly ordered");
            }
            function.sites.push_back(site);
        }
        functions.push_back(std::move(function));
    }
    std::sort(functions.begin(), functions.end(), [](const Function& a, const Function& b) { return a.startPC < b.startPC; });
    for (size_t i = 1; i < functions.size(); ++i) {
        CHECK_DETAIL(functions[i].startPC >= functions[i - 1].startPC + functions[i - 1].codeSize,
                     "conflicting AOT function extents");
    }
}

const ElfUnloadQuiescence::ImageAddressMap::Function*
ElfUnloadQuiescence::ImageAddressMap::FindFunction(Uptr pc, U16 kind) const
{
    // Caller return addresses use (entry,end]; current PCs use [entry,end).
    auto found = kind == 0
        ? std::upper_bound(functions.begin(), functions.end(), pc,
            [](Uptr value, const Function& function) { return value < function.startPC; })
        : std::lower_bound(functions.begin(), functions.end(), pc,
            [](const Function& function, Uptr value) { return function.startPC < value; });
    if (found == functions.begin()) { return nullptr; }
    --found;
    const Uptr offset = pc - found->startPC;
    return (kind == 0 ? offset < found->codeSize : offset > 0 && offset <= found->codeSize) ? &*found : nullptr;
}

ElfUnloadQuiescence::FrameMetadata ElfUnloadQuiescence::FindFrameMetadata(Uptr pc, U16 kind, Uptr entry)
{
    AssertReaderActive();
    const auto* snapshot = unloadWriter ? unloadWriterSnapshot.get() : ImageMaps().snapshot.load(std::memory_order_acquire);
    if (snapshot == nullptr) { return {}; }
    // CodeCache::find_blob -> CodeHeap::find_blob: address ownership, then
    // enumerate independent metadata registrations of that exact owner.
    const auto owner = FindImageInterval(snapshot->intervals, entry == 0 ? pc : entry, true);
    if (owner == nullptr) { return {}; }
    for (const auto& image : snapshot->images) {
        if (image->identity != owner->identity || image->ownerGeneration != owner->ownerGeneration) { continue; }
        const auto* function = entry == 0 ? image->FindFunction(pc, kind) : image->FindFunction(entry, 0);
        if (function != nullptr && (pc < function->startPC || pc - function->startPC > function->codeSize)) { continue; }
        if (function == nullptr || (entry != 0 && function->startPC != entry)) { continue; }
        const U32 offset = static_cast<U32>(pc - function->startPC);
        U16 bits = 0;
        U16 matchedKind = 0;
        QualificationMatch match = QualificationMatch::NONE;
        if (kind == 0) {
            auto transition = std::upper_bound(function->transitions.begin(), function->transitions.end(), offset,
                [](U32 value, const ImageAddressMap::Transition& row) { return value < row.offset; });
            bits = static_cast<U16>((--transition)->bits);
            match = QualificationMatch::CURRENT;
        } else {
            const auto site = std::lower_bound(function->sites.begin(), function->sites.end(), offset,
                [](const ImageAddressMap::Site& row, U32 value) { return row.offset < value; });
            auto exact = site;
            while (exact != function->sites.end() && exact->offset == offset && exact->kind != kind) { ++exact; }
            if (exact != function->sites.end() && exact->offset == offset && exact->kind == kind) {
                bits = exact->bits;
                matchedKind = exact->kind;
                match = QualificationMatch::SAVED_SITE;
            }
        }
        return {image->identity, image->ownerGeneration, image->metadata, image->generation,
                function->startPC, function->descriptor, pc, function->mapLimit, matchedKind, bits, match};
    }
    return {};
}

bool ElfUnloadQuiescence::ValidateFrameMetadata(const FrameMetadata& frame)
{
    if (frame.descriptor == 0 || frame.match == QualificationMatch::NONE) { return false; }
    const auto current = FindFrameMetadata(frame.site, frame.kind, frame.entry);
    return frame.descriptor != 0 && current.owner == frame.owner && current.ownerGeneration == frame.ownerGeneration &&
        current.metadata == frame.metadata && current.generation == frame.generation && current.descriptor == frame.descriptor &&
        current.entry == frame.entry && current.site == frame.site && current.kind == frame.kind &&
        current.match == frame.match &&
        current.bits == frame.bits && current.mapLimit == frame.mapLimit;
}

bool ElfUnloadQuiescence::ImageAddressMap::ContainsFunctionDescriptor(Uptr descriptor) const
{
    return descriptorBytes >= sizeof(MFuncDesc) && descriptor >= descriptorStart &&
        (descriptor - descriptorStart) % sizeof(MFuncDesc) == 0 &&
        descriptor - descriptorStart <= descriptorBytes - sizeof(MFuncDesc);

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
    ReadScope reader;
    // The writer's retiring view belongs only to its protected purge path.
    if (unloadWriter) {
        const auto image = FindImageInterval(unloadWriterIntervals, address, codeOnly);
        if (image != nullptr) { return image; }
    }
    const auto* snapshot = unloadWriter ? unloadWriterSnapshot.get() : ImageMaps().snapshot.load(std::memory_order_acquire);
    return snapshot == nullptr ? nullptr : FindImageInterval(snapshot->intervals, address, codeOnly);
}

Uptr ElfUnloadQuiescence::FindFunctionDescriptor(Uptr entry)
{
    AssertReaderActive();
    const auto frame = FindFrameMetadata(entry, 0, entry);
    return frame.descriptor;
}

Uptr ElfUnloadQuiescence::RegisteredIdentity(Uptr metadata)
{
    const auto image = RegisteredImage(metadata);
    CHECK_DETAIL(image != nullptr, "ELF image metadata was not registered: %p", reinterpret_cast<void*>(metadata));
    return image->identity;
}

ElfUnloadQuiescence::ReadScope::ReadScope(ReaderKind kind)
{
    (void)kind;
    if (kind == ReaderKind::SIGNAL_DIAGNOSTIC && unloadWriter) { return; }
    if (readerDepth != 0 || unloadWriter) {
        ++readerDepth;
        active = true;
        return;
    }

    auto& state = State();
    for (;;) {
        U64 observed = state.load(std::memory_order_acquire);
        if ((observed & WRITER_BIT) != 0) {
            if (kind == ReaderKind::SIGNAL_DIAGNOSTIC) { return; }
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
    {
        auto& maps = ImageMaps();
        std::lock_guard<std::mutex> directoryLock(maps.mutex);
        const auto* snapshot = maps.snapshot.load(std::memory_order_acquire);
        if (snapshot != nullptr) { unloadWriterSnapshot.reset(new ImageSnapshot(*snapshot)); }
    }
    unloadWriter = true;
}

void ElfUnloadQuiescence::UnloadScope::Synchronize()
{
    std::unique_lock<std::mutex> lock(DrainMutex());
    DrainCondition().wait(lock, []() {
        return (State().load(std::memory_order_acquire) & READER_MASK) == 0;
    });
    // zUnload.cpp:126-173: retire metadata only after the existing reader drain.
    {
        auto& maps = ImageMaps();
        std::lock_guard<std::mutex> directoryLock(maps.mutex);
        maps.retired.clear();
    }
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
    unloadWriterSnapshot.reset();
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

std::shared_ptr<ElfUnloadQuiescence::ImageAddressMap> ElfUnloadQuiescence::CaptureImage(Uptr imageAddress)
{
    // Capture once while the image is being registered, before runtime locks.
    // Dependency Begin and pending checks must not call into the platform
    // loader while dlclose is waiting in an image's fini callback.
    auto image = std::make_shared<ImageAddressMap>();
    image->metadata = imageAddress;
    image->identity = ResolveImageIdentity(imageAddress);
    CHECK_DETAIL(image->identity != 0, "ELF load image identity is unavailable");

#if defined(_WIN64)
    Uptr cursor = image->identity;
    MEMORY_BASIC_INFORMATION info {};
    constexpr DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    while (VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info)) != 0 &&
           reinterpret_cast<Uptr>(info.AllocationBase) == image->identity) {
        if (info.State == MEM_COMMIT && (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0) {
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
                if ((segment->initprot & VM_PROT_READ) != 0 && segment->vmsize != 0) {
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
            if (segment.p_type == PT_LOAD && segment.p_memsz != 0 && (segment.p_flags & PF_R) != 0) {
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
    return image;
}

std::shared_ptr<const ElfUnloadQuiescence::ImageAddressMap> ElfUnloadQuiescence::LinkImage(Uptr imageAddress)
{
    auto image = CaptureImage(imageAddress);
    CHECK_DETAIL(!IsImageClosing(imageAddress), "ELF load cannot reopen a closing image generation");
    image->ValidateMetadata();
    CHECK_DETAIL(readerDepth == 0 && !unloadWriter, "metadata registration cannot run inside a reader or unload");
    std::unique_lock<std::mutex> publicationLock(WriterMutex());
    CHECK_DETAIL(!IsImageClosing(imageAddress), "metadata publisher cannot reopen a closing owner");
    const U64 previous = State().fetch_or(WRITER_BIT, std::memory_order_acq_rel);
    CHECK_DETAIL((previous & WRITER_BIT) == 0, "metadata publishers must be serialized");
    {
    auto& maps = ImageMaps();
    std::lock_guard<std::mutex> lock(maps.mutex);
    static std::atomic<U64> nextGeneration { 1 };
    image->generation = nextGeneration.fetch_add(1, std::memory_order_relaxed);
    image->ownerGeneration = image->generation;
    for (const auto& registered : maps.images) {
        CHECK_DETAIL(registered->metadata != imageAddress, "metadata is already registered");
        if (registered->identity != image->identity) { continue; }
        image->ownerGeneration = registered->ownerGeneration;
        for (const auto& function : image->functions) {
            for (const auto& old : registered->functions) {
                CHECK_DETAIL((function.descriptor == old.descriptor && function.startPC == old.startPC && function.codeSize == old.codeSize) ||
                             function.startPC >= old.startPC + old.codeSize ||
                             old.startPC >= function.startPC + function.codeSize,
                             "conflicting function metadata in one owner");
            }
        }
    }
    maps.images.push_back(image);
    maps.Rebuild();
    maps.Publish();
    }
    // Publish and retire first; the same unload reader drain protects old
    // snapshots during normal registration, without a second epoch mechanism.
    {
        std::unique_lock<std::mutex> drainLock(DrainMutex());
        DrainCondition().wait(drainLock, []() { return (State().load(std::memory_order_acquire) & READER_MASK) == 0; });
    }
    {
        auto& maps = ImageMaps();
        std::lock_guard<std::mutex> directoryLock(maps.mutex);
        maps.retired.clear();
    }
    State().fetch_and(READER_MASK, std::memory_order_release);
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
    maps.Publish();
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
