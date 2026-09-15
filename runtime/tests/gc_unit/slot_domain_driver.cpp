// Real runtime reservation and MCC consumers; no range publication or MCC substitutes.
#include "Cangjie.h"
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
    MArray* holder = MCC_NewObjArray(types.array, 8);
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
    if (!payload || g_cjHeapRangeCount != 2) {
        std::printf("SLOT_DOMAIN_SETUP_FAIL payload=%p ranges=%lu\n", payload, g_cjHeapRangeCount);
        return 90;
    }
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
    // Occupy the legal domain, leaving two 64 MiB windows for the native
    // reservation search. No product map/range/backend is replaced. A 128 MiB
    // contiguous reservation cannot fit, so TryMapMemory must use its fallback.
    constexpr size_t segment = 64UL * 1024 * 1024;
    const uintptr_t domain = ZAddressHeapBase;
    const size_t size = ZAddressOffsetMax;
    const size_t granule = 1UL << 21;
    const size_t step = (((size - segment) / 8192 + granule - 1) / granule) * granule;
    void* occupied = mmap(reinterpret_cast<void*>(domain), size, PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (occupied == MAP_FAILED) { std::perror("domain reservation"); return 93; }
    if (munmap(reinterpret_cast<void*>(domain), segment) != 0 ||
        munmap(reinterpret_cast<void*>(domain + step), segment) != 0) return 94;
    std::printf("SLOT_DOMAIN_INPUT domain=%#lx size=%zu windows=%#lx,%#lx segment=%zu\n",
                domain, size, domain, domain + step, segment);
    (void)setenv("cjProcessorNum", "1", 1);
    (void)setenv("cjGCInterval", "3600s", 1);
    RuntimeParam param {};
    param.heapParam.heapSize = 128 * 1024;
    param.coParam.processorNum = 1;
    if (InitCJRuntime(&param) != E_OK) return 95;
    auto& manager = MutatorManager::Instance();
    manager.CreateRuntimeMutator(ThreadType::GC_THREAD);
    Mutator::GetMutator()->SetManagedContext(false);
    const int result = RunConsumers();
    manager.DestroyRuntimeMutator(ThreadType::GC_THREAD);
    if (FiniCJRuntime() != E_OK) return 96;
    return result;
}
