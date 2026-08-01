// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_OBJECT_PTR_H
#define MRT_OBJECT_PTR_H

#include <type_traits>

#include "Base/Macros.h"

namespace MapleRuntime {
class BaseObject;
class Collector;
class CollectorProxy;
class Mutator;
class WCollector;

// A managed-object address whose current-version property has not been established.
class MaybeStalePtr {
public:
    constexpr MaybeStalePtr() : pointer(nullptr) {}
    explicit constexpr MaybeStalePtr(BaseObject* object) : pointer(object) {}

private:
    BaseObject* pointer;

    friend class Collector;
    friend class CollectorProxy;
    friend class Mutator;
    friend class WCollector;
};

// A managed-object address produced by a current-version canonicalizer, an
// explicitly audited UnsafeAssumeCurrent call, or audited carrier checks.
class CurrentPtr {
public:
    // Dropping the current-version proof does not create one, so this direction
    // is intentionally implicit. The reverse direction has no conversion.
    ALWAYS_INLINE constexpr operator BaseObject*() const { return pointer; }

private:
    explicit constexpr CurrentPtr(BaseObject* object) : pointer(object) {}

    BaseObject* pointer;

    friend class Collector;
    friend class CollectorProxy;
    friend class Mutator;
    friend class WCollector;
    friend CurrentPtr ProvenByCarrierChecks(BaseObject* object);
    friend CurrentPtr ProvenByRegionWalk(BaseObject* object);
    friend CurrentPtr ProvenByFreshAllocation(BaseObject* object);
    friend CurrentPtr ProvenByPinnedSlot(BaseObject* object);
    friend CurrentPtr ProvenByNonOldBranch(BaseObject* object);
    friend CurrentPtr ProvenByTypedReceiver(BaseObject* object);
    friend CurrentPtr ProvenByBarrierRead(BaseObject* object);
    friend CurrentPtr ProvenByWorkStack(BaseObject* object);
    friend CurrentPtr UnsafeAssumeCurrent(BaseObject* object);
};

ALWAYS_INLINE inline CurrentPtr UnsafeAssumeCurrent(BaseObject* object) { return CurrentPtr(object); }
// Region linear/live walks only yield allocated current-version objects.
ALWAYS_INLINE inline CurrentPtr ProvenByRegionWalk(BaseObject* object) { return CurrentPtr(object); }
// Fresh heap allocations complete type init before any concurrent collector can retire them.
ALWAYS_INLINE inline CurrentPtr ProvenByFreshAllocation(BaseObject* object) { return CurrentPtr(object); }
// Free pinned slots retain a live object header under the slot-list lifecycle.
ALWAYS_INLINE inline CurrentPtr ProvenByPinnedSlot(BaseObject* object) { return CurrentPtr(object); }
// Explicit non-old / current-tag branch already proved the referent is current.
ALWAYS_INLINE inline CurrentPtr ProvenByNonOldBranch(BaseObject* object) { return CurrentPtr(object); }
// Member body reached only through a CurrentPtr friend wrapper (receiver already typed).
ALWAYS_INLINE inline CurrentPtr ProvenByTypedReceiver(BaseObject* object) { return CurrentPtr(object); }
// Barrier ReadStaticRef/ReadReference resolves to the current version before returning.
ALWAYS_INLINE inline CurrentPtr ProvenByBarrierRead(BaseObject* object) { return CurrentPtr(object); }
// Work-stack entries are only pushed after enum/tag/region proof established currentness.
ALWAYS_INLINE inline CurrentPtr ProvenByWorkStack(BaseObject* object) { return CurrentPtr(object); }

using ObjectPtr = BaseObject*;

static_assert(std::is_trivially_copyable<MaybeStalePtr>::value, "MaybeStalePtr must remain trivially copyable");
static_assert(std::is_trivially_copyable<CurrentPtr>::value, "CurrentPtr must remain trivially copyable");
static_assert(sizeof(MaybeStalePtr) == sizeof(BaseObject*), "MaybeStalePtr must remain pointer-sized");
static_assert(sizeof(CurrentPtr) == sizeof(BaseObject*), "CurrentPtr must remain pointer-sized");
static_assert(alignof(MaybeStalePtr) == alignof(BaseObject*), "MaybeStalePtr must retain pointer alignment");
static_assert(alignof(CurrentPtr) == alignof(BaseObject*), "CurrentPtr must retain pointer alignment");
} // namespace MapleRuntime

#endif // MRT_OBJECT_PTR_H
