#include "Heap/Allocator/HeapFiller.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sys/mman.h>

#include "Base/Panic.h"
#include "Common/BaseObject.h"
#include "ObjectModel/Flags.h"
#include "ObjectModel/MClass.h"
#include "TypeInfoManager.h"
#include "securec.h"

namespace MapleRuntime {
namespace HeapFiller {

static TypeInfo* g_unitTi = nullptr;
static TypeInfo* g_arrayTi = nullptr;
static TypeInfo* g_byteTi = nullptr;
static std::atomic<bool> g_typesReady{ false };

bool Enabled()
{
    const char* v = std::getenv("CJRT_HEAP_FILLER");
    return v != nullptr && v[0] == '1' && v[1] == '\0';
}

static TypeInfo* PlantTi(void* storage, TypeKind kind, U32 sizeOrComp, const char* name)
{
    std::memset(storage, 0, sizeof(TypeInfo));
    auto* ti = reinterpret_cast<TypeInfo*>(storage);
    ti->SetType(kind);
    ti->SetInstanceSize(sizeOrComp);
    ti->SetAlign(8);
    ti->SetName(name);
    GCTib gctib {};
    gctib.tag = SIGN_BIT;
    ti->SetGCTib(gctib);
    return ti;
}

static void EnsureTypes()
{
    if (g_typesReady.load(std::memory_order_acquire)) {
        return;
    }
    static std::mutex mu;
    std::lock_guard<std::mutex> lock(mu);
    if (g_typesReady.load(std::memory_order_relaxed)) {
        return;
    }
    constexpr size_t kBytes = 4096;
    void* page = mmap(nullptr, kBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        return;
    }
    auto* base = static_cast<char*>(page);
    g_byteTi = PlantTi(base, TypeKind::TYPE_KIND_UINT8, 1, "FillerByte");
    g_unitTi = PlantTi(base + 256, TypeKind::TYPE_KIND_CLASS, 0, "FillerUnit");
    g_arrayTi = PlantTi(base + 512, TypeKind::TYPE_KIND_RAWARRAY, 1, "FillerArray");
    g_arrayTi->SetComponentTypeInfo(g_byteTi);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(page), kBytes);
    g_typesReady.store(true, std::memory_order_release);
}

bool IsFiller(const BaseObject* obj)
{
    if (obj == nullptr || !g_typesReady.load(std::memory_order_acquire)) {
        return false;
    }
    TypeInfo* ti = obj->GetTypeInfo();
    return ti == g_unitTi || ti == g_arrayTi;
}

static void Overlay(uintptr_t start, size_t size)
{
    EnsureTypes();
    if (g_unitTi == nullptr || (size & 7u) != 0 || size < 8) {
        return;
    }
    auto* obj = reinterpret_cast<BaseObject*>(start);
    if (size == 8) {
        obj->SetClassInfo(g_unitTi);
        return;
    }
    obj->SetClassInfo(g_arrayTi);
    *reinterpret_cast<MIndex*>(start + sizeof(void*)) = size - 16u;
}

void ZeroAndFill(uintptr_t start, size_t size)
{
    if (size == 0) {
        return;
    }
    CHECK_E((memset_s(reinterpret_cast<void*>(start), size, 0, size) != EOK), "memset_s fail");
    if (Enabled()) {
        Overlay(start, size);
    }
}

} // namespace HeapFiller
} // namespace MapleRuntime
