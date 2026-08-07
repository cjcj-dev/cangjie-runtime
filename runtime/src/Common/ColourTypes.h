// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_COLOUR_TYPES_H
#define MRT_COLOUR_TYPES_H

// Colour pointer type discipline — mirror OpenJDK ZGC zAddress.hpp:247-253.
// Design truth: ops/design/COLOUR_TYPE_DISCIPLINE.md
//
// enum class has no implicit conversion ⇒ coloured / uncoloured / unsafe-uncoloured
// cannot silently flow into each other. Every state change goes through a named
// function that documents *why* the conversion is valid.
//
// Underlying type is Uptr (== MAddress). This header stays below TypeDef in the
// include graph: it must not include TypeDef.h (TypeDef pulls ColourMask and is
// included by nearly everything).

#include "Base/Types.h"

namespace MapleRuntime {

class BaseObject;

// Coloured reference — must NOT be dereferenced.
enum class zpointer : Uptr { null = 0 };

// Uncoloured — safe to dereference (barrier / proof already applied).
enum class zaddress : Uptr { null = 0 };

// Uncoloured — NOT safe to dereference; memory may be uncommitted / reclaimed.
enum class zaddress_unsafe : Uptr { null = 0 };

// ── raw bit views (for CAS expected/new, masks, logging) ──────────────────
// 凭什么: enum class stores the same bits; raw is identity, not a state change.
constexpr Uptr raw(zpointer p) { return static_cast<Uptr>(p); }
constexpr Uptr raw(zaddress a) { return static_cast<Uptr>(a); }
constexpr Uptr raw(zaddress_unsafe u) { return static_cast<Uptr>(u); }

// ── constructors from raw machine words ───────────────────────────────────
// to_zpointer: 凭什么: value was just read from a ref-field slot (or is about to
// be written into one). Only valid at the slot boundary.
constexpr zpointer to_zpointer(Uptr v) { return static_cast<zpointer>(v); }

// to_zaddress_unsafe: 凭什么: bits are already uncoloured, but the referent may
// be dead / unmapped (e.g. strip-only, or a non-heap word mistaken for a ref).
constexpr zaddress_unsafe to_zaddress_unsafe(Uptr v) { return static_cast<zaddress_unsafe>(v); }

// to_zaddress (raw): 凭什么: caller already holds a proven-good uncoloured address
// (null, or a value that went through make_load_good / safe). Prefer those.
constexpr zaddress to_zaddress(Uptr v) { return static_cast<zaddress>(v); }

// ── state transitions ─────────────────────────────────────────────────────
// safe: 凭什么: caller has *separately* proven the memory is live/mapped.
// Every call site MUST document the proof in a comment. If you cannot write the
// proof, the site is a defect — report it, do not call safe().
constexpr zaddress safe(zaddress_unsafe u) { return static_cast<zaddress>(raw(u)); }

// uncolor_bits: strip isTagged/colour high bits → address bits only, still unsafe.
// 凭什么: bit layout (ColourMask.h); does NOT run a barrier or check liveness.
constexpr zaddress_unsafe uncolor_bits(zpointer p)
{
    // address occupies bits 0..47 on 64-bit (RefField.h); ARM32 is abandoned.
    return to_zaddress_unsafe(raw(p) & ((Uptr(1) << 48) - 1u));
}

// to_object: 凭什么: sole exit from the colour type system to a C++ object pointer.
// Input must already be zaddress (load-good or proven).
// ⭐ This is the ONLY production site allowed to write reinterpret_cast<BaseObject*>.
// All other paths must go through a named constructor below (or this).
inline BaseObject* to_object(zaddress a)
{
    return reinterpret_cast<BaseObject*>(raw(a));
}

// from_object: 凭什么: a live BaseObject* in hand is already an uncoloured, safe address.
inline zaddress from_object(const BaseObject* obj)
{
    return to_zaddress(reinterpret_cast<Uptr>(obj));
}

// ── non-RefField origins of BaseObject* (ctyperest exemption set) ─────────
// Bare reinterpret_cast<BaseObject*> outside this header is forbidden
// (tools/check_no_bare_baseobject_cast.sh). Every site picks one of these
// and documents *why* the bits are an uncoloured object base.

// from_region_addr: 凭什么: address computed from region metadata
// (region start / unit / free-slot / route to-addr / walk position).
// Allocator owns the layout; bits are never colour-tagged.
inline BaseObject* from_region_addr(Uptr addr)
{
    return to_object(to_zaddress(addr));
}

// from_alloc_addr: 凭什么: address just returned by the allocator
// (SetClassInfo / NewFinalizer / AllocPinned). Memory is committed and
// about to be / just was initialised as a BaseObject; never coloured.
inline BaseObject* from_alloc_addr(Uptr addr)
{
    return to_object(to_zaddress(addr));
}

// from_native_ref: 凭什么: MCC / Sync / scheduler entry holding a managed
// object as void* / C layout view (CJFuture/CJMutex/…). Protocol: the
// mutator published an uncoloured heap ref; not a RefField load.
// ⚠ Bits are NOT run through a read barrier here — caller must not pass
// a raw field load. Stack-map base pointers (BasePtrType) use this too.
inline BaseObject* from_native_ref(const void* p)
{
    return to_object(to_zaddress(reinterpret_cast<Uptr>(p)));
}

// as_abi_ref_slot: 凭什么: stack buffer used as an ABI sret / arg-register
// placeholder in ArgValue::AddReference. ⛔ NOT a heap object; ⛔ must not
// be treated as a GC root. Exists only so the call stub sees a "ref" slot.
inline BaseObject* as_abi_ref_slot(void* p)
{
    return to_object(to_zaddress(reinterpret_cast<Uptr>(p)));
}

// null checks (enum class does not compare to 0 without cast)
constexpr bool is_null(zpointer p) { return p == zpointer::null; }
constexpr bool is_null(zaddress a) { return a == zaddress::null; }
constexpr bool is_null(zaddress_unsafe u) { return u == zaddress_unsafe::null; }

} // namespace MapleRuntime

#endif // MRT_COLOUR_TYPES_H
