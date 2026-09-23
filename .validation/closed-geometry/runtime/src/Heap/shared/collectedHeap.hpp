// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#pragma once
#include <cstdint>
namespace MapleRuntime {
class BaseObject;
// gc/shared/collectedHeap.hpp:297-317: parsable dummy objects for unused memory.
class CollectedHeap {
public:
    static void fill_with_dummy_object(uintptr_t start, uintptr_t end, bool zap);
    static bool is_filler_object(const BaseObject* obj);
};
}
