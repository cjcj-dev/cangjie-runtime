// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "Cangjie.h"
#include "Heap/z/zHeap.hpp"
#include "Common/ScopedObjectAccess.h"
#include "Heap/z/zIterator.inline.hpp"
#include "Heap/z/zTask.hpp"
#include "Heap/z/zWorkers.hpp"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/MArray.inline.h"
#include "ObjectModel/MObject.h"
#include <cstdio>
#include <cstring>
using namespace MapleRuntime;

class VerifyFunnelTask : public ZTask {
    std::function<void()> run;
public:
    explicit VerifyFunnelTask(std::function<void()> function) : ZTask("VerifyFunnelInput"), run(function) {}
    void work() override { if (WorkerThread::worker_id() == 0) { run(); } }
};

int main(int argc, char** argv)
{
    if (argc != 2) { return 78; }
    const bool iterator = std::strncmp(argv[1], "iterator-", 9) == 0;
    const bool saferegion = std::strstr(argv[1], "saferegion") != nullptr;
    const bool gcworker = std::strstr(argv[1], "gcworker") != nullptr;
    const bool worldstopped = std::strstr(argv[1], "worldstopped") != nullptr;
    RuntimeParam param{};
    param.coParam.processorNum = 1;
    param.heapParam.heapSize = 32 * 1024;
    if (InitCJRuntime(&param) != E_OK) { return 79; }
    auto& manager = MutatorManager::Instance();
    Mutator* mutator = manager.CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    if (mutator == nullptr) { return 80; }
    alignas(TypeInfo) unsigned char objectStorage[sizeof(TypeInfo)]{};
    alignas(TypeInfo) unsigned char arrayStorage[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(objectStorage);
    auto* arrayType = reinterpret_cast<TypeInfo*>(arrayStorage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetInstanceSize(sizeof(uintptr_t));
    arrayType->SetType(TypeKind::TYPE_KIND_RAWARRAY);
    arrayType->SetComponentTypeInfo(type);
    BaseObject* target;
    MArray* array = nullptr;
    ObjectRef* root = nullptr;
    {
        ScopedObjectAccess access;
        target = MObject::NewPinnedObject(type, 2 * sizeof(uintptr_t));
        if (target == nullptr) { return 81; }
        if (iterator) {
            array = MArray::NewRefArray(2, *arrayType, AllocType::MOVEABLE_OBJECT);
            if (array == nullptr) { return 82; }
            for (size_t index = 0; index < 2; ++index) {
                auto& field = HeapSlotAt<>(reinterpret_cast<MAddress>(array) +
                                          MArray::GetContentOffset() + index * sizeof(uintptr_t));
                field.StoreColoured(ZAddress::store_good(from_object(target)));
            }
        } else {
            root = mutator->AddNativeFrameRoot(target);
        }
    }
    size_t fields = 0;
    bool valuesMatch = true;
    if (FILE* maps = std::fopen("/proc/self/maps", "r")) {
        char line[1024];
        while (std::fgets(line, sizeof(line), maps) != nullptr) { std::fputs(line, stderr); }
        std::fclose(maps);
    }
    const auto run = [&] {
        std::fprintf(stderr, "DEBUG_FUNNEL_STATE mode=%s gc=%d stopped=%d saferegion=%d\n", argv[1],
                     ThreadLocal::GetThreadType() == ThreadType::GC_THREAD,
                     manager.WorldStopped(), mutator->InSaferegion());
        if (iterator) {
            auto visitor = [&](RefField<>& field) {
                ++fields;
                valuesMatch = valuesMatch && ZPointer::uncolor(field.GetFieldValue()) == from_object(target);
            };
            // This exact specialization is extern-template in zIterator.hpp:
            // its implementation is imported from the product, not instantiated here.
            ZIterator::basic_oop_iterate_safe(array, visitor);
        } else {
            valuesMatch = mutator->GcPhaseEnum(false, 0, true, nullptr) &&
                raw(root->LoadPlain()) == reinterpret_cast<uintptr_t>(target);
        }
    };
    if (gcworker) {
        VerifyFunnelTask task(run);
        Heap::GetHeap().old().Workers()->run(&task);
    } else if (worldstopped) {
        ScopedStopTheWorld stopped("VerifyFunnelInput");
        run();
    } else if (saferegion) {
        (void)mutator->EnterSaferegion(false);
        run();
    } else {
        ScopedObjectAccess access;
        run();
    }
    const bool matched = valuesMatch && (!iterator || fields == 2);
    std::fprintf(stderr, "DEBUG_FUNNEL_RESULT mode=%s fields=%zu matched=%d\n", argv[1], fields, matched);
    if (root != nullptr) { mutator->PopNativeFrameRootsTo(0); }
    manager.DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    return matched ? 0 : 83;
}
