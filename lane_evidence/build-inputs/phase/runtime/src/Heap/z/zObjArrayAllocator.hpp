// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#pragma once
#include "Common/TypeDef.h"
namespace MapleRuntime {
class TypeInfo;
class MArray;
class ZObjArrayAllocator {
public:
    ZObjArrayAllocator(MAddress address, MSize size, MIndex length, TypeInfo& klass)
        : address(address), arraySize(size), nElems(length), arrayClass(klass) {}
    MArray* initialize();
private:
    void yield_for_safepoint() const;
    MAddress address;
    MSize arraySize;
    MIndex nElems;
    TypeInfo& arrayClass;
};
}
