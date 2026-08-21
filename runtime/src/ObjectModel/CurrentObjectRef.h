// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_CURRENT_OBJECT_REF_H
#define MRT_CURRENT_OBJECT_REF_H

#include <cstddef>
#include <type_traits>

namespace MapleRuntime {

class BaseObject;
class Collector;
class WCollector;

// Current-version object pointer. Mirror OpenJDK zaddress (zAddress.hpp): a
// value that has gone through resolve-or-forward, as opposed to a maybe-from
// BaseObject* (zaddress_unsafe analogue).
//
// Wrong-coloured / from-version hand-out cannot compile once consumers take
// this type (phase ②). Phase ① lands the type only; product signatures stay
// BaseObject*.
//
// Slot-colour discipline is HeapSlot/RootSlot/DerivedSlot in RefField.h.
// This type is the *object* side of the same contract.
class CurrentObjectRef {
public:
    CurrentObjectRef() = default;
    CurrentObjectRef(const CurrentObjectRef&) = default;
    CurrentObjectRef(CurrentObjectRef&&) = default;
    CurrentObjectRef& operator=(const CurrentObjectRef&) = default;
    CurrentObjectRef& operator=(CurrentObjectRef&&) = default;
    ~CurrentObjectRef() = default;

    CurrentObjectRef(BaseObject*) = delete;
    CurrentObjectRef(const BaseObject*) = delete;
    CurrentObjectRef& operator=(BaseObject*) = delete;
    CurrentObjectRef& operator=(const BaseObject*) = delete;
    operator BaseObject*() const = delete;

    // 凭什么: caller already resolved (make_load_good / FindToVersion / relocate).
    // Only resolve-or-forward paths may mint. A maybe-from BaseObject* is not
    // convertible — that is the fence.
    static CurrentObjectRef fromResolved(BaseObject* obj)
    {
        CurrentObjectRef ref;
        ref.ptr = obj;
        return ref;
    }

    static CurrentObjectRef null() { return CurrentObjectRef(); }

    // Escape hatch. Contract: bits are the current-version object or null;
    // caller is leaving the typed world (MCC ABI, existing BaseObject* APIs).
    BaseObject* unsafeRaw() const { return ptr; }

    bool isNull() const { return ptr == nullptr; }

    bool operator==(CurrentObjectRef other) const { return ptr == other.ptr; }
    bool operator!=(CurrentObjectRef other) const { return ptr != other.ptr; }

private:
    BaseObject* ptr = nullptr;

    friend class Collector;
    friend class WCollector;
};

static_assert(sizeof(CurrentObjectRef) == sizeof(void*), "CurrentObjectRef must remain one machine word");
static_assert(!std::is_constructible<CurrentObjectRef, BaseObject*>::value,
              "maybe-from BaseObject* must not construct CurrentObjectRef");
static_assert(!std::is_convertible<BaseObject*, CurrentObjectRef>::value,
              "maybe-from BaseObject* must not convert to CurrentObjectRef");
static_assert(!std::is_convertible<CurrentObjectRef, BaseObject*>::value,
              "CurrentObjectRef must not silently decay to BaseObject*");

} // namespace MapleRuntime
#endif // MRT_CURRENT_OBJECT_REF_H
