// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#pragma once
#include "Heap/shared/memAllocator.hpp"
namespace MapleRuntime {
class ZObjArrayAllocator : public ObjArrayAllocator {
public:
    ZObjArrayAllocator(TypeInfo& klass, MSize size, MIndex length, bool doZero)
        : ObjArrayAllocator(klass, size, length, doZero) {}
private:
    MArray* initialize(MAddress address) const override;
    void yield_for_safepoint() const;
};
}
