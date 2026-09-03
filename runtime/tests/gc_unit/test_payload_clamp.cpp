#include <array>
#include <cstdint>
#include <cstring>
#include <csignal>
#include <functional>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

#include "gc_heap_fixture.hpp"

#include "Common/ColourPredicates.h"
#include "Heap/Barrier/Barrier.h"
#define private public
#include "Heap/Barrier/RememberedSet.h"
#undef private
#include "Heap/Collector/Collector.h"
#include "Heap/Heap.h"
#include "Heap/WCollector/IdleBarrier.h"
#include "ObjectModel/MArray.inline.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

extern "C" void CJ_MCC_WriteGenericPayload(MapleRuntime::ObjectPtr dst, MapleRuntime::MAddress srcField,
                                           size_t srcSize);
extern "C" void CJ_MCC_ReadGenericPayload(void* dstNative, MapleRuntime::ObjectPtr obj, size_t size);

namespace {

class NoAnswerCollector final : public Collector {
public:
    void Init() override {}
    void RunGarbageCollection(uint64_t, GCReason) override {}
    bool ShouldIgnoreRequest(GCRequest&) override { return false; }
    FindToVersionResult FindToVersion(BaseObject*) const override { return FindToVersionResult::NotForwarded(); }
    bool TryUpdateRefField(BaseObject*, RefField<>&, BaseObject*&) const override { return false; }
    bool IsOldPointer(RefField<>&) const override { return false; }
    bool IsFromObject(BaseObject*) const override { return false; }
    bool IsGhostFromObject(BaseObject*) const override { return false; }
    bool IsUnmovableFromObject(BaseObject*) const override { return false; }
    RefField<> GetAndTryTagRefField(BaseObject* object) const override
    {
        const uintptr_t remap = ColourPredicates::current_remapped(static_cast<uintptr_t>(::g_cjLoadBadMask));
        return RefField<>(GcUnit::ColouredPointer(object, remap));
    }
    ZGenerationId remap_generation(RefField<>&) const override { return ZGenerationId::old; }
    BaseObject* relocate_or_remap_object(BaseObject* object, ZGenerationId) const override { return object; }
};

class InstalledBarrierScope {
public:
    explicit InstalledBarrierScope(Barrier& barrier) : previous(Heap::currentBarrierPtr), installed(&barrier)
    {
        Heap::currentBarrierPtr = &installed;
    }
    ~InstalledBarrierScope() { Heap::currentBarrierPtr = previous; }

private:
    Barrier** previous;
    Barrier* installed;
};

struct PayloadFixture {
    PayloadFixture() : barrier(collector, rememberedSet), installed(barrier)
    {
        rememberedSet.Initialize(heap.heapStart, GcHeapFixture::kUnits * RegionInfo::UNIT_SIZE);
        auto& heapRemset = Heap::GetHeap().GetRememberedSet();
        if (!heapRemset.initialized) {
            heapRemset.Initialize(heap.heapStart, GcHeapFixture::kUnits * RegionInfo::UNIT_SIZE);
        }
        heap.typeInfo->SetFlag(0);
        heap.typeInfo->SetInstanceSize(sizeof(uint64_t));
    }

    GcHeapFixture heap;
    NoAnswerCollector collector;
    RememberedSet rememberedSet;
    IdleBarrier barrier;
    InstalledBarrierScope installed;
};

struct ArrayPayloadFixture {
    ArrayPayloadFixture() : payload()
    {
        std::memset(componentStorage, 0, sizeof(componentStorage));
        component = reinterpret_cast<TypeInfo*>(componentStorage);
        component->SetType(TypeKind::TYPE_KIND_UINT8);
        component->SetInstanceSize(sizeof(uint8_t));

        std::memset(arrayStorage, 0, sizeof(arrayStorage));
        arrayType = reinterpret_cast<TypeInfo*>(arrayStorage);
        arrayType->SetType(TypeKind::TYPE_KIND_RAWARRAY);
        arrayType->SetComponentTypeInfo(component);
        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
            reinterpret_cast<uintptr_t>(componentStorage), sizeof(componentStorage));
        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
            reinterpret_cast<uintptr_t>(arrayStorage), sizeof(arrayStorage));

        array = reinterpret_cast<MArray*>(payload.heap.obj1);
        array->SetClassInfo(arrayType);
        array->SetLength(kLength);
        limit = array->GetMArraySize() - TYPEINFO_PTR_SIZE;
    }

    static constexpr MIndex kLength = 32;
    PayloadFixture payload;
    alignas(TypeInfo) unsigned char componentStorage[sizeof(TypeInfo)];
    alignas(TypeInfo) unsigned char arrayStorage[sizeof(TypeInfo)];
    TypeInfo* component = nullptr;
    TypeInfo* arrayType = nullptr;
    MArray* array = nullptr;
    size_t limit = 0;
};

std::array<uint8_t, 64> MakeArrayPayload(const ArrayPayloadFixture& fx)
{
    std::array<uint8_t, 64> bytes{};
    for (size_t i = 0; i < fx.limit; ++i) {
        bytes[i] = static_cast<uint8_t>(0x40U + i);
    }
    const MIndex length = ArrayPayloadFixture::kLength;
    std::memcpy(bytes.data(), &length, sizeof(length));
    return bytes;
}

void ExpectControlledAbort(const std::function<void()>& body, const std::string& expectedDiagnostic = {})
{
    int stderrPipe[2] = { -1, -1 };
    if (!expectedDiagnostic.empty()) {
        GC_EXPECT_EQ(pipe(stderrPipe), 0);
    }
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        if (!expectedDiagnostic.empty()) {
            (void)close(stderrPipe[0]);
            (void)dup2(stderrPipe[1], STDERR_FILENO);
            (void)close(stderrPipe[1]);
        }
        (void)signal(SIGABRT, SIG_DFL);
        body();
        _exit(0);
    }
    std::string stderrOutput;
    if (!expectedDiagnostic.empty()) {
        (void)close(stderrPipe[1]);
        std::array<char, 256> chunk{};
        ssize_t bytes = 0;
        while ((bytes = read(stderrPipe[0], chunk.data(), chunk.size())) > 0) {
            stderrOutput.append(chunk.data(), static_cast<size_t>(bytes));
        }
        (void)close(stderrPipe[0]);
    }
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    GC_EXPECT_TRUE(WIFSIGNALED(status));
    GC_EXPECT_EQ(WTERMSIG(status), SIGABRT);
    if (!expectedDiagnostic.empty()) {
        GC_EXPECT_TRUE(stderrOutput.find(expectedDiagnostic) != std::string::npos);
    }
}

} // namespace

GC_TEST(PayloadClamp, ReadAtLimitCopies)
{
    PayloadFixture fx;
    const size_t limit = fx.heap.typeInfo->GetInstanceSize();
    const uint64_t expected = 0x1122334455667788ULL;
    std::memcpy(reinterpret_cast<void*>(reinterpret_cast<MAddress>(fx.heap.obj1) + TYPEINFO_PTR_SIZE),
                &expected, sizeof(expected));
    uint64_t got = 0;
    CJ_MCC_ReadGenericPayload(&got, fx.heap.obj1, limit);
    GC_EXPECT_EQ(got, expected);
}

GC_TEST(PayloadClamp, WriteAtLimitCopies)
{
    PayloadFixture fx;
    const size_t limit = fx.heap.typeInfo->GetInstanceSize();
    const uint64_t expected = 0xaabbccddeeff0011ULL;
    CJ_MCC_WriteGenericPayload(fx.heap.obj1, reinterpret_cast<MAddress>(&expected), limit);
    uint64_t got = 0;
    std::memcpy(&got, reinterpret_cast<void*>(reinterpret_cast<MAddress>(fx.heap.obj1) + TYPEINFO_PTR_SIZE),
                sizeof(got));
    GC_EXPECT_EQ(got, expected);
}

GC_OTHER_VM_TEST(PayloadClamp, ReadOverLimitAborts)
{
    PayloadFixture fx;
    const size_t limit = fx.heap.typeInfo->GetInstanceSize();
    uint64_t buf[2] = {};
    ExpectControlledAbort([&]() { CJ_MCC_ReadGenericPayload(buf, fx.heap.obj1, limit + 1); });
}

GC_OTHER_VM_TEST(PayloadClamp, WriteOverLimitAborts)
{
    PayloadFixture fx;
    const size_t limit = fx.heap.typeInfo->GetInstanceSize();
    uint64_t src[2] = { 1, 2 };
    ExpectControlledAbort([&]() {
        CJ_MCC_WriteGenericPayload(fx.heap.obj1, reinterpret_cast<MAddress>(src), limit + 1);
    });
}

GC_TEST(PayloadClamp, ReadArrayAtLimitCopies)
{
    ArrayPayloadFixture fx;
    const auto expected = MakeArrayPayload(fx);
    std::memcpy(reinterpret_cast<void*>(reinterpret_cast<MAddress>(fx.array) + TYPEINFO_PTR_SIZE),
                expected.data(), fx.limit);
    std::array<uint8_t, 64> got{};
    CJ_MCC_ReadGenericPayload(got.data(), fx.array, fx.limit);
    GC_EXPECT_EQ(std::memcmp(got.data(), expected.data(), fx.limit), 0);
}

GC_TEST(PayloadClamp, WriteArrayAtLimitCopies)
{
    ArrayPayloadFixture fx;
    const auto expected = MakeArrayPayload(fx);
    CJ_MCC_WriteGenericPayload(fx.array, reinterpret_cast<MAddress>(expected.data()), fx.limit);
    std::array<uint8_t, 64> got{};
    std::memcpy(got.data(), reinterpret_cast<void*>(reinterpret_cast<MAddress>(fx.array) + TYPEINFO_PTR_SIZE),
                fx.limit);
    GC_EXPECT_EQ(std::memcmp(got.data(), expected.data(), fx.limit), 0);
}

GC_OTHER_VM_TEST(PayloadClamp, ReadArrayOverLimitAborts)
{
    ArrayPayloadFixture fx;
    std::array<uint8_t, 64> buf{};
    ExpectControlledAbort([&]() { CJ_MCC_ReadGenericPayload(buf.data(), fx.array, fx.limit + 1); },
                          "object payload " + std::to_string(fx.limit));
}

GC_OTHER_VM_TEST(PayloadClamp, WriteArrayOverLimitAborts)
{
    ArrayPayloadFixture fx;
    std::array<uint8_t, 64> src{};
    ExpectControlledAbort([&]() {
        CJ_MCC_WriteGenericPayload(fx.array, reinterpret_cast<MAddress>(src.data()), fx.limit + 1);
    }, "object payload " + std::to_string(fx.limit));
}
