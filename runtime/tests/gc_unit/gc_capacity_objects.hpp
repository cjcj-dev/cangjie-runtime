// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#pragma once
#include <algorithm>
#include <vector>
#include <deque>
#include "ObjectModel/MObject.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zRootsIterator.hpp"
#include "TypeInfoManager.h"

namespace MapleRuntime {
extern "C" ObjRef MCC_NewObject(const TypeInfo*, MSize);
}
namespace MapleRuntime::GcUnit {
// Capacity is not a class layout. Keep each real class allocation within the
// existing U32 ABI, and retain every object while the director/allocator runs.
inline std::vector<U64> AllocateRootedCapacity(size_t bytes, bool pinned)
{
    constexpr size_t chunkBytes = size_t{1} << 30;
    struct alignas(TypeInfo) TypeStorage { unsigned char bytes[sizeof(TypeInfo)]{}; };
    // Each call keeps its layouts stable, including a differently sized tail
    // in a subsequent allocation-rate burst. deque preserves image addresses.
    static std::deque<TypeStorage> images;
    images.emplace_back();
    auto& fullStorage = images.back().bytes;
    images.emplace_back();
    auto& tailStorage = images.back().bytes;
    auto* fullType = reinterpret_cast<TypeInfo*>(fullStorage);
    auto* tailType = reinterpret_cast<TypeInfo*>(tailStorage);
    fullType->SetType(TypeKind::TYPE_KIND_CLASS);
    fullType->SetInstanceSize(chunkBytes - TYPEINFO_PTR_SIZE);
    tailType->SetType(TypeKind::TYPE_KIND_CLASS);
    const size_t tailBytes = bytes % chunkBytes;
    GC_EXPECT_TRUE(tailBytes == 0 || (tailBytes >= TYPEINFO_PTR_SIZE && tailBytes % sizeof(uintptr_t) == 0));
    if (tailBytes != 0) { tailType->SetInstanceSize(tailBytes - TYPEINFO_PTR_SIZE); }
    auto& types = TypeInfoManager::GetTypeInfoManager();
    types.NoteTypeInfoImage(reinterpret_cast<uintptr_t>(fullStorage), sizeof(fullStorage));
    types.NoteTypeInfoImage(reinterpret_cast<uintptr_t>(tailStorage), sizeof(tailStorage));
    auto& heap = Heap::GetHeap();
    std::vector<U64> roots;
    const size_t allocatedBefore = heap.page_allocator().GetAllocatedSize();
    size_t total = 0;
    while (total < bytes) {
        const size_t size = std::min(chunkBytes, bytes - total);
        auto* type = size == chunkBytes ? fullType : tailType;
        auto* object = pinned ? MObject::NewPinnedObject(type, static_cast<MSize>(size)) :
                              reinterpret_cast<MObject*>(MCC_NewObject(type, static_cast<MSize>(size)));
        GC_EXPECT_TRUE(object != nullptr);
        roots.push_back(heap.cross_vm().export_roots().RegisterExportRoot(object));
        total += size;
    }
    std::fprintf(stderr, "CAPACITY_OBJECTS_TARGET bytes=%zu roots=%zu allocated_before=%zu allocated_after=%zu pinned=%d\n",
        total, roots.size(), allocatedBefore, heap.page_allocator().GetAllocatedSize(), pinned);
    GC_EXPECT_EQ(total, bytes);
    return roots;
}
}
