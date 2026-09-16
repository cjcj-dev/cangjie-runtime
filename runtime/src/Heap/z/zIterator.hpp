// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#ifndef MRT_Z_ITERATOR_HPP
#define MRT_Z_ITERATOR_HPP

#include "Common/BaseObject.h"
#include "ObjectModel/MArray.h"

namespace MapleRuntime {

// VM adapter: Cangjie has HeapSlot fields and no narrowOop representation.
// ZGC memory/iterator.hpp: OopClosure / BasicOopIterateClosure.
class OopClosure {
public:
    virtual ~OopClosure() = default;
    virtual void do_oop(RefField<>* field) = 0;
};

class BasicOopIterateClosure : public OopClosure {};

class OopIteratorClosureDispatch {
public:
    template <typename OopClosureT>
    static void oop_oop_iterate(OopClosureT* closure, BaseObject* object, TypeInfo* klass);
};

// ZGC zIterator.hpp:35-64: the safe boundary belongs to the iterator,
// independently of the VM field layout and of mark's array chunking.
class ZIterator {
private:
    ZIterator() = delete;
    static bool is_invisible_object(BaseObject* object);
    static bool is_invisible_object_array(BaseObject* object);
    static bool is_invisible_object_array(BaseObject* object, TypeInfo* klass);

public:
    template <typename OopClosureT>
    static void oop_iterate_safe(BaseObject* object, OopClosureT* closure);
    template <typename OopClosureT>
    static void oop_iterate_safe(BaseObject* object, TypeInfo* klass, OopClosureT* closure);
    template <typename OopClosureT>
    static void oop_iterate(BaseObject* object, OopClosureT* closure);
    template <typename OopClosureT>
    static void oop_iterate_elements_range(MArray* object, OopClosureT* closure, MIndex start, MIndex end);

    template <typename Function>
    static void basic_oop_iterate_safe(BaseObject* object, Function function);
    template <typename Function>
    static void basic_oop_iterate_safe(BaseObject* object, TypeInfo* klass, Function function);
    template <typename Function>
    static void basic_oop_iterate(BaseObject* object, Function function);
};

template <typename Function>
class ZBasicOopIterateClosure : public BasicOopIterateClosure {
private:
    Function _function;
public:
    explicit ZBasicOopIterateClosure(Function function) : _function(function) {}
    void do_oop(RefField<>* field) override { _function(*field); }
};

class ObjectClosure {
public:
    virtual ~ObjectClosure() = default;
    virtual void do_object(BaseObject* object) = 0;
};

template <typename Function>
class ZObjectClosure : public ObjectClosure {
    Function function;
public:
    explicit ZObjectClosure(Function function) : function(function) {}
    void do_object(BaseObject* object) override { function(object); }
};

// The runtime's type-erased field visitor is also used by promotion workers.
// Keep those instantiations in the product so callers link the same entry.
extern template void ZIterator::oop_iterate_safe<ZBasicOopIterateClosure<RefFieldVisitor>>(
    BaseObject*, ZBasicOopIterateClosure<RefFieldVisitor>*);
extern template void ZIterator::oop_iterate_safe<ZBasicOopIterateClosure<RefFieldVisitor>>(
    BaseObject*, TypeInfo*, ZBasicOopIterateClosure<RefFieldVisitor>*);
extern template void ZIterator::oop_iterate<ZBasicOopIterateClosure<RefFieldVisitor>>(
    BaseObject*, ZBasicOopIterateClosure<RefFieldVisitor>*);
extern template void ZIterator::oop_iterate_elements_range<ZBasicOopIterateClosure<RefFieldVisitor>>(
    MArray*, ZBasicOopIterateClosure<RefFieldVisitor>*, MIndex, MIndex);
extern template void ZIterator::basic_oop_iterate_safe<RefFieldVisitor>(BaseObject*, RefFieldVisitor);
extern template void ZIterator::basic_oop_iterate_safe<RefFieldVisitor>(BaseObject*, TypeInfo*, RefFieldVisitor);
extern template void ZIterator::basic_oop_iterate<RefFieldVisitor>(BaseObject*, RefFieldVisitor);

} // namespace MapleRuntime
#endif
