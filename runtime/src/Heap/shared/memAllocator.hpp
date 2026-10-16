// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#pragma once
#include "Common/TypeDef.h"
namespace MapleRuntime {
class TypeInfo;
class MArray;
// HotSpot gc/shared/memAllocator.hpp:37-105.
class MemAllocator {
public:
    MArray* allocate() const;
    virtual ~MemAllocator() = default;
protected:
    MemAllocator(TypeInfo& klass, size_t size) : arrayClass(klass), arraySize(size) {}
    virtual MArray* initialize(MAddress address) const = 0;
    void mem_clear(MAddress address) const;
    MArray* finish(MAddress address) const;
    TypeInfo& arrayClass;
    const size_t arraySize;
};
class ObjArrayAllocator : public MemAllocator {
public:
    ObjArrayAllocator(TypeInfo& klass, size_t size, MIndex length, bool zero)
        : MemAllocator(klass, size), nElems(length), doZero(zero) {}
protected:
    MArray* initialize(MAddress address) const override;
    const MIndex nElems;
    const bool doZero;
};
}
