// Real runtime reservation and MCC consumers; no range publication or MCC substitutes.
#include <list>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <vector>
#include "Cangjie.h"
// Test setup claims virtual space through the real manager so a normal large
// array allocation reaches the ninth reservation. No range provider is replaced.
#define private public
#include "Heap/Allocator/RegionSpace.h"
#undef private
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zHeap.hpp"
#include "Mutator/Mutator.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/MArray.inline.h"
#include "TypeInfoManager.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>

using namespace MapleRuntime;
extern "C" void slot_domain_write(void*, void*, void**);
extern "C" void* slot_domain_read(void*, void**);
namespace MapleRuntime {
extern "C" ArrayRef MCC_NewObjArray(const TypeInfo*, MIndex);
}

static NativeSlot globalSlot(zpointer::null);
static const char* selected = "all";
static unsigned failures = 0;
static unsigned assertions = 0;
static size_t requestedReservations = 2;
static size_t targetReservation = 0;

static void Expect(const char* name, uintptr_t actual, uintptr_t expected)
{
    ++assertions;
    const bool pass = actual == expected;
    failures += !pass;
    std::printf("SLOT_DOMAIN_ASSERT name=%s actual=%#lx expected=%#lx pass=%d\n",
                name, actual, expected, pass);
}

struct ArrayTypes {
    alignas(TypeInfo) unsigned char componentBytes[sizeof(TypeInfo)] {};
    alignas(TypeInfo) unsigned char arrayBytes[sizeof(TypeInfo)] {};
    TypeInfo* array;
    ArrayTypes()
    {
        auto* component = reinterpret_cast<TypeInfo*>(componentBytes);
        component->SetType(TypeKind::TYPE_KIND_CLASS);
        component->SetInstanceSize(sizeof(void*));
        array = reinterpret_cast<TypeInfo*>(arrayBytes);
        array->SetType(TypeKind::TYPE_KIND_RAWARRAY);
        array->SetComponentTypeInfo(component);
        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(this), sizeof(*this));
    }
};

static bool Enabled(const char* name)
{
    return std::strcmp(selected, "all") == 0 || std::strcmp(selected, name) == 0;
}

static int RunConsumers()
{
    ArrayTypes types;
    ZArray<ZVirtualMemory> borrowed;
    auto& space = static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    if (targetReservation != 0) {
        // Temporarily own the other free virtual ranges. MCC_NewObjArray still
        // executes the product claim/commit/map path and creates the real holder.
        ZArray<ZVirtualMemory> free;
        space.virtualMemory->remove_from_low_many_at_most(
            ZAddressOffsetMax, ZPerNUMAStorage::id(), &free);
        for (const auto& range : free) {
            const uintptr_t start = untype(ZOffset::address_unsafe(range.start()));
            if (start >= g_cjHeapRangeStart[targetReservation] &&
                start + range.size() <= g_cjHeapRangeEnd[targetReservation]) {
                space.virtualMemory->insert(range, ZPerNUMAStorage::id());
            } else {
                borrowed.append(range);
            }
        }
    }
    // A four MiB array bypasses any existing small-page allocation buffer.
    MArray* holder = MCC_NewObjArray(types.array, targetReservation == 0 ? 8 : 512 * 1024);
    for (const auto& range : borrowed) {
        space.virtualMemory->insert(range, ZPerNUMAStorage::id());
    }
    const uintptr_t holderAddress = reinterpret_cast<uintptr_t>(holder);
    if (holderAddress < g_cjHeapRangeStart[targetReservation] ||
        holderAddress >= g_cjHeapRangeEnd[targetReservation]) {
        std::printf("SLOT_DOMAIN_TARGET_SETUP_FAIL holder=%p target=%zu\n", holder, targetReservation);
        return 98;
    }
    // Choose an actual allocated object whose raw bits satisfy the load mask.
    // That makes a mistaken heap fast read of plain storage observably shift it.
    MArray* payload = nullptr;
    for (unsigned i = 0; i < 8192; ++i) {
        auto* object = MCC_NewObjArray(types.array, 1);
        if ((reinterpret_cast<uintptr_t>(object) & ZPointerLoadBadMask) == 0) {
            payload = object;
            break;
        }
    }
    if (!payload || g_cjHeapRangeCount != requestedReservations) {
        std::printf("SLOT_DOMAIN_SETUP_FAIL payload=%p ranges=%lu\n", payload, g_cjHeapRangeCount);
        return 90;
    }
    Expect("reservation.count", g_cjHeapRangeCount, requestedReservations);
    std::printf("SLOT_DOMAIN_TARGET_RESERVATION index=%zu start=%#lx end=%#lx holder=%p\n",
                targetReservation, g_cjHeapRangeStart[targetReservation],
                g_cjHeapRangeEnd[targetReservation], holder);
    auto** heapSlot = reinterpret_cast<void**>(holder->ConvertToCArray());
    const uintptr_t hole = g_cjHeapRangeEnd[0];
    void* mapped = mmap(reinterpret_cast<void*>(hole), 4096, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (mapped == MAP_FAILED || hole >= g_cjHeapRangeStart[1] || Heap::IsHeapAddress(mapped)) {
        std::printf("SLOT_DOMAIN_SETUP_FAIL hole=%#lx\n", hole);
        return 91;
    }
    auto** plainSlot = static_cast<void**>(mapped);
    NativeSlot* roots[] = { &globalSlot };
    Heap::GetHeap().RegisterStaticRoots(reinterpret_cast<Uptr>(roots), 1);
    const uintptr_t address = reinterpret_cast<uintptr_t>(payload);
    const uintptr_t colored = raw(ZAddress::store_good(to_zaddress(address)));
    const uintptr_t coloredNull = raw(ZAddress::store_good(zaddress::null));
    std::printf("SLOT_DOMAIN_SETUP ranges=%lu first=%#lx-%#lx second=%#lx-%#lx heap=%p hole=%p global=%p payload=%p\n",
                g_cjHeapRangeCount, g_cjHeapRangeStart[0], g_cjHeapRangeEnd[0],
                g_cjHeapRangeStart[1], g_cjHeapRangeEnd[1], heapSlot, plainSlot, &globalSlot, payload);
    auto exercise = [&](const char* name, void* owner, void** slot, bool isPlain) {
        if (!Enabled(name)) return;
        // Each target starts independently: a bad write cannot hide a read failure.
        *reinterpret_cast<uintptr_t*>(slot) = isPlain ? 0 : coloredNull;
        slot_domain_write(payload, owner, slot);
        char label[80];
        std::snprintf(label, sizeof(label), "%s.write", name);
        Expect(label, *reinterpret_cast<uintptr_t*>(slot), isPlain ? address : colored);
        *reinterpret_cast<uintptr_t*>(slot) = isPlain ? address : colored;
        void* result = slot_domain_read(owner, slot);
        std::snprintf(label, sizeof(label), "%s.read", name);
        Expect(label, reinterpret_cast<uintptr_t>(result), address);
    };
    exercise("heap_dyn", holder, heapSlot, false);
    exercise("heap_null", nullptr, heapSlot, false);
    exercise("hole", holder, plainSlot, true);
    exercise("global", reinterpret_cast<void*>(1), reinterpret_cast<void**>(&globalSlot), false);
    Heap::GetHeap().UnregisterStaticRoots(reinterpret_cast<Uptr>(roots), 1);
    std::printf("SLOT_DOMAIN_RESULT assertions=%u failures=%u\n", assertions, failures);
    return assertions == 0 ? 92 : failures == 0 ? 0 : 1;
}

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc > 1) selected = argv[1];
    ZGlobalsPointers::initialize();
    if (const char* value = std::getenv("SLOT_DOMAIN_RESERVATIONS")) {
        requestedReservations = std::strtoul(value, nullptr, 10);
    }
    if (requestedReservations != 2 && requestedReservations != 9) return 99;
    targetReservation = requestedReservations == 9 ? 8 : 0;
    // Occupy the legal domain and leave exact separated windows. The real
    // ZVirtualMemoryReserver must take its recursive fallback; the real
    // RegionSpace::Init publishes the resulting reservations to LLVM.
    const size_t segment = requestedReservations == 9 ? 32 * 1024 * 1024 : 64 * 1024 * 1024;
    const uintptr_t domain = ZAddressHeapBase;
    const size_t size = ZAddressOffsetMax;
    void* occupied = mmap(reinterpret_cast<void*>(domain), size, PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (occupied == MAP_FAILED) { std::perror("domain reservation"); return 93; }
    for (size_t i = 0; i < requestedReservations; ++i) {
        if (munmap(reinterpret_cast<void*>(domain + 2 * i * segment), segment) != 0) return 94;
    }
    std::printf("SLOT_DOMAIN_INPUT domain=%#lx size=%zu windows=%zu segment=%zu\n",
                domain, size, requestedReservations, segment);
    (void)setenv("cjProcessorNum", "1", 1);
    (void)setenv("cjGCInterval", "3600s", 1);
    RuntimeParam param {};
    param.heapParam.heapSize = 128 * 1024;
    param.coParam.processorNum = 1;
    if (InitCJRuntime(&param) != E_OK) return 95;
    if (const char* path = std::getenv("SLOT_DOMAIN_MAPS")) {
        FILE* source = std::fopen("/proc/self/maps", "r");
        FILE* output = std::fopen(path, "w");
        if (!source || !output) return 97;
        char line[4096];
        while (std::fgets(line, sizeof(line), source)) std::fputs(line, output);
        std::fclose(source);
        std::fclose(output);
    }
    auto& manager = MutatorManager::Instance();
    manager.CreateRuntimeMutator(ThreadType::GC_THREAD);
    Mutator::GetMutator()->SetManagedContext(false);
    const int result = RunConsumers();
    manager.DestroyRuntimeMutator(ThreadType::GC_THREAD);
    if (FiniCJRuntime() != E_OK) return 96;
    return result;
}
