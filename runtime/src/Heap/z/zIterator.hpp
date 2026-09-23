// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#ifndef MRT_Z_ITERATOR_HPP
#define MRT_Z_ITERATOR_HPP

#include "Common/BaseObject.h"
#include "ObjectModel/MArray.h"
#include "Heap/z/zReferenceDiscoverer.hpp"

namespace MapleRuntime {

// VM adapter: Cangjie has HeapSlot fields and no narrowOop representation.
// ZGC memory/iterator.hpp: OopClosure / BasicOopIterateClosure.
class OopClosure {
public:
    virtual ~OopClosure() = default;
    virtual void do_oop(RefField<>* field) = 0;
};

// HotSpot memory/iterator.hpp:69-92: reference processing belongs to the
// closure, independently of the VM's field bitmap representation.
class OopIterateClosure : public OopClosure {
    ReferenceDiscoverer* _ref_discoverer;
protected:
    explicit OopIterateClosure(ReferenceDiscoverer* rd) : _ref_discoverer(rd) {}
    OopIterateClosure() : _ref_discoverer(nullptr) {}
    void set_ref_discoverer_internal(ReferenceDiscoverer* rd) { _ref_discoverer = rd; }
public:
    ReferenceDiscoverer* ref_discoverer() const { return _ref_discoverer; }
    enum ReferenceIterationMode {
        DO_DISCOVERY,
        DO_FIELDS,
        DO_FIELDS_EXCEPT_REFERENT
    };
    virtual ReferenceIterationMode reference_iteration_mode() { return DO_DISCOVERY; }
};

class BasicOopIterateClosure : public OopIterateClosure {
public:
    explicit BasicOopIterateClosure(ReferenceDiscoverer* rd = nullptr) : OopIterateClosure(rd) {}
};

class OopIteratorClosureDispatch {
    static BaseObject* load_referent(BaseObject* object, ReferenceType type);
    template <typename OopClosureT>
    static bool try_discover(BaseObject* object, ReferenceType type, OopClosureT* closure);
    template <typename OopClosureT>
    static void oop_oop_iterate_ref_processing(OopClosureT* closure, BaseObject* object);
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

class OopFieldClosure {
public:
    virtual ~OopFieldClosure() = default;
    virtual void do_field(BaseObject* base, const void* slot, uintptr_t value) = 0;
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

} // namespace MapleRuntime
#endif
