// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#include "Heap/shared/stringdedup/stringDedup.hpp"
#include <cstring>
#include <limits>
#include <random>
#include <vector>
#include "Common/SuspendibleThreadSet.h"
#include "Common/WeakHandle.inline.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zThread.hpp"
#include "ObjectModel/MArray.inline.h"

namespace MapleRuntime {
namespace {
// stringDedupConfig.cpp:61-139 and globals.hpp:1862-1883. Only the table
// sizing policy applies to the explicit String value-type API.
struct DedupConfig {
    inline static constexpr size_t sizes[] = {
        503, 751, 1009, 1511, 2003, 3001, 4001, 6007, 8009, 12007, 16001, 24001,
        32003, 48017, 64007, 96001, 128021, 192007, 256019, 384001, 512009, 768013,
        1024021, 1536011, 2048003, 3072001, 4096013, 6144001, 8192003, 12288011,
        16384001, 24576001, 32768011, 49152001, 65536043, 98304053,
        131072003, 196608007, 262144009, 393216007, 524288057, 786432001,
        1048576019, 1572864001
    };
    static size_t GoodSize(size_t size)
    {
        for (size_t candidate : sizes) { if (size <= candidate) return candidate; }
        return sizes[sizeof(sizes) / sizeof(sizes[0]) - 1];
    }
    static size_t GrowThreshold(size_t size)
    {
        return size == GoodSize(SIZE_MAX) ? SIZE_MAX : static_cast<size_t>(size * 14.0);
    }
    static size_t ShrinkThreshold(size_t size) { return size == sizes[0] ? 0 : size; }
    static size_t DesiredTableSize(size_t entries) { return GoodSize(static_cast<size_t>(entries / 7.0)); }
    static bool ShouldCleanup(size_t entries, size_t dead)
    {
        return dead > 100 && dead > entries * 0.05;
    }
};
}

// stringDedupTable.cpp:90-180. Parallel vectors preserve the upstream bucket
// shape; std::vector is the C++ storage adapter for GrowableArrayCHeap.
class StringDedup::Table::Bucket {
    std::vector<uint32_t> hashes;
    std::vector<WeakHandle> values;
    void ExpandIfFull()
    {
        if (hashes.size() == hashes.capacity()) {
            const size_t capacity = NeededCapacity(hashes.capacity() + 1);
            hashes.reserve(capacity);
            values.reserve(capacity);
        }
    }
public:
    void Initialize(size_t reserve)
    {
        hashes.reserve(reserve);
        values.reserve(reserve);
    }
    ~Bucket()
    {
        for (WeakHandle& value : values) value.release(&StringDedup::Instance().WeakStorage());
    }
    static size_t NeededCapacity(size_t needed)
    {
        if (needed == 0) return 0;
        size_t high = 1;
        while (high < needed) high *= 2;
        const size_t low = high - high / 4;
        return needed <= low ? low : high;
    }
    bool IsEmpty() const { return hashes.empty(); }
    size_t Length() const { return hashes.size(); }
    uint32_t LastHash() const { return hashes.back(); }
    WeakHandle LastValue() const { return values.back(); }
    WeakHandle ValueAt(size_t index) const { return values[index]; }
    void Add(uint32_t hash, WeakHandle value)
    {
        ExpandIfFull();
        hashes.push_back(hash);
        values.push_back(value);
    }
    void DeleteAt(size_t index)
    {
        values[index].release(&StringDedup::Instance().WeakStorage());
        // GrowableArray::delete_at replaces the removed entry with the last.
        hashes[index] = hashes.back();
        values[index] = values.back();
        PopNoRelease();
    }
    void PopNoRelease() { hashes.pop_back(); values.pop_back(); }
    void Shrink() { hashes.shrink_to_fit(); values.shrink_to_fit(); }
    WeakHandle Find(MArray* object, uint32_t hash) const
    {
        for (size_t index = 0; index < hashes.size(); ++index) {
            if (hashes[index] != hash) continue;
            auto* value = static_cast<MArray*>(values[index].peek());
            if (value != nullptr && value->GetLength() == object->GetLength() &&
                (value == object || std::memcmp(value->ConvertToCArray(), object->ConvertToCArray(),
                                                object->GetLength()) == 0)) {
                return values[index];
            }
        }
        return WeakHandle();
    }
};

class StringDedup::Table::CleanupState {
public:
    virtual ~CleanupState() = default;
    virtual bool Step() = 0;
    virtual WeakHandle Find(MArray* object, uint32_t hash) const = 0;
};

// stringDedupTable.cpp:293-355: transfer one entry, then shrink one bucket.
class StringDedup::Table::Resizer final : public CleanupState {
    Table& table;
    std::unique_ptr<Bucket[]> buckets;
    size_t numberOfBuckets;
    size_t bucketIndex = 0;
    size_t shrinkIndex;
public:
    Resizer(Table& table, bool growOnly, std::unique_ptr<Bucket[]> oldBuckets, size_t count)
        : table(table), buckets(std::move(oldBuckets)), numberOfBuckets(count),
          shrinkIndex(growOnly ? table.numberOfBuckets : 0)
    {
        table.needBucketShrinking = !growOnly;
    }
    bool Step() override
    {
        if (bucketIndex < numberOfBuckets) {
            Bucket& bucket = buckets[bucketIndex];
            if (bucket.IsEmpty()) {
                bucket.Shrink();
                ++bucketIndex;
            } else {
                const uint32_t hash = bucket.LastHash();
                WeakHandle value = bucket.LastValue();
                bucket.PopNoRelease();
                if (value.peek() != nullptr) table.Add(value, hash);
                else value.release(&table.storage);
            }
            return true;
        }
        if (shrinkIndex < table.numberOfBuckets) {
            table.buckets[shrinkIndex++].Shrink();
            return true;
        }
        return false;
    }
    WeakHandle Find(MArray* object, uint32_t hash) const override
    {
        return buckets[hash % numberOfBuckets].Find(object, hash);
    }
};

// stringDedupTable.cpp:371-416: retain the index when removing a dead entry.
class StringDedup::Table::Cleaner final : public CleanupState {
    Table& table;
    size_t bucketIndex = 0;
    size_t entryIndex = 0;
public:
    explicit Cleaner(Table& table) : table(table) { table.needBucketShrinking = false; }
    bool Step() override
    {
        if (bucketIndex == table.numberOfBuckets) return false;
        Bucket& bucket = table.buckets[bucketIndex];
        CHECK_DETAIL(entryIndex <= bucket.Length(), "dedup cleaner index");
        if (entryIndex == bucket.Length()) {
            bucket.Shrink();
            ++bucketIndex;
            entryIndex = 0;
        } else if (bucket.ValueAt(entryIndex).peek() == nullptr) {
            bucket.DeleteAt(entryIndex);
            --table.numberOfEntries;
        } else {
            ++entryIndex;
        }
        return true;
    }
    WeakHandle Find(MArray*, uint32_t) const override { return WeakHandle(); }
};

StringDedup::Table::Table(StringDedup& owner) : owner(owner)
{
    // initialize_storage/initialize (stringDedupTable.cpp:431-441). The
    // containing singleton is stable before any root task captures the set.
    storage.register_num_dead_callback(NumDeadCallback);
}
StringDedup::Table::~Table() = default;

std::unique_ptr<StringDedup::Table::Bucket[]> StringDedup::Table::MakeBuckets(size_t count, size_t reserve)
{
    auto result = std::make_unique<Bucket[]>(count);
    for (size_t index = 0; index < count; ++index) result[index].Initialize(reserve);
    return result;
}

void StringDedup::Table::Initialize()
{
    numberOfBuckets = DedupConfig::GoodSize(500);
    buckets = MakeBuckets(numberOfBuckets);
    growThreshold = DedupConfig::GrowThreshold(numberOfBuckets);
    numberOfEntries = 0;
    needBucketShrinking = false;
    std::lock_guard<std::mutex> guard(owner.monitor);
    deadCount.store(0, std::memory_order_relaxed);
    deadState.store(DeadState::good, std::memory_order_release);
}

void StringDedup::Table::Clear()
{
    cleanupState.reset();
    buckets.reset();
    numberOfEntries = 0;
    numberOfBuckets = 0;
    std::lock_guard<std::mutex> guard(owner.monitor);
    deadCount.store(0, std::memory_order_relaxed);
    deadState.store(DeadState::wait2, std::memory_order_release);
}

void StringDedup::Table::Add(WeakHandle value, uint32_t hash)
{
    buckets[hash % numberOfBuckets].Add(hash, value);
    ++numberOfEntries;
}

WeakHandle StringDedup::Table::Find(MArray* object, uint32_t hash) const
{
    if (cleanupState != nullptr) {
        WeakHandle value = cleanupState->Find(object, hash);
        if (!value.is_empty()) return value;
    }
    return buckets[hash % numberOfBuckets].Find(object, hash);
}

void StringDedup::Table::Install(MArray* object, uint32_t hash)
{
    Add(WeakHandle(&storage, object), hash);
}

bool StringDedup::Table::IsDeadCountGoodAcquire() const
{
    return deadState.load(std::memory_order_acquire) == DeadState::good;
}

bool StringDedup::Table::IsGrowNeeded() const
{
    return IsDeadCountGoodAcquire() && numberOfEntries - deadCount.load(std::memory_order_relaxed) > growThreshold;
}

bool StringDedup::Table::IsDeadEntryRemovalNeeded() const
{
    return IsDeadCountGoodAcquire() && DedupConfig::ShouldCleanup(numberOfEntries, deadCount.load(std::memory_order_relaxed));
}

// stringDedupTable.cpp:244-270. This callback runs after old weak processing;
// it neither takes table ownership nor reads a heap object.
void StringDedup::Table::NumDeadCallback(size_t count)
{
    auto& dedup = StringDedup::Instance();
    auto& table = dedup.table;
    std::lock_guard<std::mutex> guard(dedup.monitor);
    switch (table.deadState.load(std::memory_order_relaxed)) {
        case DeadState::good:
            table.deadCount.store(count, std::memory_order_relaxed);
            break;
        case DeadState::wait1:
            table.deadCount.store(count, std::memory_order_relaxed);
            table.deadState.store(DeadState::good, std::memory_order_release);
            break;
        case DeadState::wait2:
            table.deadState.store(DeadState::wait1, std::memory_order_release);
            break;
        case DeadState::cleaning:
            break;
    }
    dedup.workPending = true;
    dedup.condition.notify_all();
}

void StringDedup::Table::SetDeadStateCleaning()
{
    std::lock_guard<std::mutex> guard(owner.monitor);
    deadCount.store(0, std::memory_order_relaxed);
    deadState.store(DeadState::cleaning, std::memory_order_relaxed);
}

bool StringDedup::Table::StartResizer(bool growOnly, size_t entries)
{
    const size_t newCount = DedupConfig::DesiredTableSize(entries);
    auto newBuckets = MakeBuckets(newCount, Bucket::NeededCapacity(entries / newCount));
    cleanupState = std::make_unique<Resizer>(*this, growOnly, std::move(buckets), numberOfBuckets);
    buckets = std::move(newBuckets);
    numberOfBuckets = newCount;
    numberOfEntries = 0;
    growThreshold = DedupConfig::GrowThreshold(newCount);
    SetDeadStateCleaning();
    return true;
}

bool StringDedup::Table::StartCleaner()
{
    cleanupState = std::make_unique<Cleaner>(*this);
    SetDeadStateCleaning();
    return true;
}

bool StringDedup::Table::CleanupStartIfNeeded(bool growOnly, bool force)
{
    CHECK_DETAIL(cleanupState == nullptr, "dedup cleanup already active");
    if (!IsDeadCountGoodAcquire()) return false;
    const size_t dead = deadCount.load(std::memory_order_relaxed);
    CHECK_DETAIL(dead <= numberOfEntries, "dedup dead count exceeds entries");
    const size_t adjusted = numberOfEntries - dead;
    if (force || adjusted > DedupConfig::GrowThreshold(numberOfBuckets)) {
        return StartResizer(growOnly, adjusted);
    }
    if (growOnly) return false;
    if (adjusted < DedupConfig::ShrinkThreshold(numberOfBuckets)) return StartResizer(false, adjusted);
    if (needBucketShrinking || DedupConfig::ShouldCleanup(numberOfEntries, dead)) return StartCleaner();
    return false;
}

bool StringDedup::Table::CleanupStep()
{
    CHECK_DETAIL(cleanupState != nullptr, "dedup cleanup missing");
    return cleanupState->Step();
}

void StringDedup::Table::CleanupEnd()
{
    CHECK_DETAIL(cleanupState != nullptr, "dedup cleanup missing");
    cleanupState.reset();
    std::lock_guard<std::mutex> guard(owner.monitor);
    deadState.store(DeadState::wait2, std::memory_order_relaxed);
}

// StringDedupProcessor.cpp:69-115,177-188. There is no HotSpot JavaThread
// carrier here. The existing STS supplies its between-step safepoint boundary.
class StringDedup::Processor final : public ZThread {
    StringDedup& owner;
    template<class Operation>
    bool WithTable(Operation operation)
    {
        while (!should_terminate()) {
            {
                SuspendibleThreadSetJoiner joined;
                std::unique_lock<std::mutex> lock(owner.tableMutex, std::try_to_lock);
                if (lock.owns_lock()) return operation();
            }
            // Neither join STS while owning the table nor wait for the table
            // while joined. Relocation workers never acquire this table lock.
            std::lock_guard<std::mutex> wait(owner.tableMutex);
        }
        return false;
    }
    void CleanupTable(bool growOnly, bool force)
    {
        if (!WithTable([&] { return owner.table.CleanupStartIfNeeded(growOnly, force); })) return;
        while (WithTable([&] { return owner.table.CleanupStep(); })) {}
        WithTable([&] { owner.table.CleanupEnd(); return true; });
    }
    bool WaitForRequests()
    {
        std::unique_lock<std::mutex> lock(owner.monitor);
        owner.condition.wait(lock, [&] { return should_terminate() || owner.workPending; });
        owner.workPending = false;
        const bool requests = owner.requestsPending;
        owner.requestsPending = false;
        return requests;
    }
public:
    explicit Processor(StringDedup& owner) : owner(owner)
    {
        set_name("StringDedup");
        create_and_start();
    }
    void run_thread() override
    {
        while (!should_terminate()) {
            const bool requests = WaitForRequests();
            if (should_terminate()) break;
            // ProcessRequest's grow branch precedes ordinary cleanup. With
            // no requests, the weak report wakes only the dead-removal path.
            if (requests) {
                if (WithTable([&] { return owner.table.IsGrowNeeded(); })) CleanupTable(true, false);
                CleanupTable(false, false);
            } else if (WithTable([&] { return owner.table.IsDeadEntryRemovalNeeded(); })) {
                CleanupTable(false, false);
            }
        }
    }
    void terminate() override
    {
        std::lock_guard<std::mutex> lock(owner.monitor);
        owner.condition.notify_all();
    }
};

StringDedup::StringDedup() : table(*this)
{
    std::random_device random;
    hashSeed = (static_cast<uint64_t>(random()) << 32) | random();
}
StringDedup::~StringDedup() { Stop(); }

StringDedup& StringDedup::Instance()
{
    static StringDedup instance;
    return instance;
}

void StringDedup::Start()
{
    std::lock_guard<std::mutex> lifecycle(lifecycleMutex);
    std::lock_guard<std::mutex> lock(tableMutex);
    if (!stopped) return;
    table.Initialize();
    stopped = false;
    processor = std::make_unique<Processor>(*this);
}

void StringDedup::Stop()
{
    std::lock_guard<std::mutex> lifecycle(lifecycleMutex);
    {
        std::lock_guard<std::mutex> lock(tableMutex);
        stopped = true;
    }
    if (processor != nullptr) {
        processor->stop();
        processor.reset();
    }
    // Runtime shutdown calls this after root tasks and GC workers have exited.
    std::lock_guard<std::mutex> lock(tableMutex);
    table.Clear();
}

void StringDedup::NotifyWork()
{
    std::lock_guard<std::mutex> lock(monitor);
    requestsPending = true;
    workPending = true;
    condition.notify_all();
}
bool StringDedup::Accepts(const TypeInfo* arrayInfo, ArrayRef candidate)
{
    // zStringDedup.inline.hpp:38 requires String identity, not byte-array type.
    // The explicit ABI is that identity: only a full UInt8 RawArray is installed.
    if (candidate == nullptr || arrayInfo == nullptr || !Heap::IsHeapAddress(candidate)) {
        return false;
    }
    if (candidate->GetTypeInfo() != arrayInfo || !candidate->IsRawArray()) {
        return false;
    }
    TypeInfo* component = candidate->GetComponentTypeInfo();
    if (component == nullptr || component->GetType() != TypeKind::TYPE_KIND_UINT8) {
        return false;
    }
    return candidate->GetLength() != 0;
}

size_t StringDedup::Hash(BaseObject* object) const
{
    // StringDedupTable::compute_hash / AltHashing::halfsiphash_32: HalfSipHash-2-4.
    auto* array = static_cast<MArray*>(object);
    const uint8_t* bytes = array->ConvertToCArray();
    const size_t length = array->GetLength();
    uint32_t a = static_cast<uint32_t>(hashSeed);
    uint32_t b = static_cast<uint32_t>(hashSeed >> 32);
    uint32_t c = a ^ 0x6c796765U;
    uint32_t d = b ^ 0x74656462U;
    auto rotate = [](uint32_t value, unsigned count) {
        return (value << count) | (value >> (32 - count));
    };
    auto rounds = [&](unsigned count) {
        while (count-- != 0) {
            a += b; b = rotate(b, 5) ^ a; a = rotate(a, 16);
            c += d; d = rotate(d, 8) ^ c;
            a += d; d = rotate(d, 7) ^ a;
            c += b; b = rotate(b, 13) ^ c; c = rotate(c, 16);
        }
    };
    auto absorb = [&](uint32_t word) { d ^= word; rounds(2); a ^= word; };
    size_t offset = 0;
    while (length - offset >= 4) {
        uint32_t word = 0;
        for (unsigned byte = 0; byte != 4; ++byte) word |= uint32_t(bytes[offset++]) << (8 * byte);
        absorb(word);
    }
    uint32_t tail = static_cast<uint32_t>(length) << 24;
    for (unsigned byte = 0; offset != length; ++byte) tail |= uint32_t(bytes[offset++]) << (8 * byte);
    absorb(tail);
    c ^= 0xffU;
    rounds(4);
    return b ^ d;
}

ArrayRef StringDedup::Canonical(const TypeInfo* arrayInfo, ArrayRef candidate)
{
    if (!Accepts(arrayInfo, candidate)) return candidate;
    // The mutator remains outside a saferegion throughout find -> resolve.
    // There is no monitor wait, safepoint or ownership release between them.
    std::lock_guard<std::mutex> lock(tableMutex);
    if (stopped) return candidate;
    const uint32_t hash = Hash(candidate);
    WeakHandle value = table.Find(candidate, hash);
    if (value.is_empty()) {
        table.Install(candidate, hash);
        NotifyWork();
        return candidate;
    }
    auto* found = static_cast<MArray*>(value.resolve());
    CHECK_DETAIL(found != nullptr, "dedup resolved entry must remain live");
    NotifyWork();
    return found;
}
} // namespace MapleRuntime
