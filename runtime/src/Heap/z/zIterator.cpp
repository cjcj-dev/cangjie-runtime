// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#include "Heap/z/zIterator.inline.hpp"

namespace MapleRuntime {
template void ZIterator::oop_iterate_safe<ZBasicOopIterateClosure<RefFieldVisitor>>(
    BaseObject*, ZBasicOopIterateClosure<RefFieldVisitor>*);
template void ZIterator::oop_iterate_safe<ZBasicOopIterateClosure<RefFieldVisitor>>(
    BaseObject*, TypeInfo*, ZBasicOopIterateClosure<RefFieldVisitor>*);
template void ZIterator::oop_iterate<ZBasicOopIterateClosure<RefFieldVisitor>>(
    BaseObject*, ZBasicOopIterateClosure<RefFieldVisitor>*);
template void ZIterator::oop_iterate_elements_range<ZBasicOopIterateClosure<RefFieldVisitor>>(
    MArray*, ZBasicOopIterateClosure<RefFieldVisitor>*, MIndex, MIndex);
template void ZIterator::basic_oop_iterate_safe<RefFieldVisitor>(BaseObject*, RefFieldVisitor);
template void ZIterator::basic_oop_iterate_safe<RefFieldVisitor>(BaseObject*, TypeInfo*, RefFieldVisitor);
template void ZIterator::basic_oop_iterate<RefFieldVisitor>(BaseObject*, RefFieldVisitor);
} // namespace MapleRuntime
