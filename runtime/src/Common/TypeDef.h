// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_TYPE_DEF_H
#define MRT_TYPE_DEF_H

#include <limits>
#include <climits>

#include "Base/Macros.h"
#include "Base/Types.h"

// commonly agreed type interfaces for a managed runtime:
//    they're opaque across modules, but we still want it provides a degree
//    of type safety.
namespace MapleRuntime {
// Those are mostly managed pointer types for GC
using MAddress = Uptr; // Managed address
constexpr Uptr NULL_ADDRESS = 0;

// Tag-ID generation count for WCollector phase tags (RefField tagID field).
// Default 3: after skipping postflip tag-fix (乙), stale tags live until the next
// major TRACE heals them; N=2 would wrap "one gen behind" into current.
// Override with -DMRT_TAG_ID_COUNT=N to rebuild other widths.
#ifndef MRT_TAG_ID_COUNT
#define MRT_TAG_ID_COUNT 3
#endif
constexpr uint16_t TAG_ID_COUNT = static_cast<uint16_t>(MRT_TAG_ID_COUNT);
// Bits needed for values in [0, TAG_ID_COUNT). Taken from RefField padding on 64-bit.
constexpr unsigned TAG_ID_BITS =
    (TAG_ID_COUNT <= 2) ? 1u : (TAG_ID_COUNT <= 4) ? 2u : (TAG_ID_COUNT <= 8) ? 3u : 4u;
// address:48 + isTagged:1 + tagID:TAG_ID_BITS + padding:TAG_ID_PADDING_BITS == 64
constexpr unsigned TAG_ID_PADDING_BITS = 15u - TAG_ID_BITS;

// object model related types
class BaseObject;

// basic types for managed world: modify them together
using MSize = U32;   // managed object size
using MOffset = U32; // managed offset inside managed object
using MIndex = U64;  // index of array
constexpr size_t GENERIC_PAYLOAD_SIZE = INT_MAX; // only for CJ_MCC_WriteGenericPayload

constexpr MIndex MAX_ARRAY_SIZE = std::numeric_limits<MIndex>::max();

class MObject;
using ObjRef = MObject*;

class MArray;
using ArrayRef = MArray*;

class MFunc;
using FuncRef = MFunc*;

class MFuncDesc;
using FuncDescRef = MFuncDesc*;

class MString;
using StringRef = MString*;

using MException = MObject;
using ExceptionRef = MException*;

class ExceptionWrapper;

class MethodInfo;
using MethodInfoRef = MethodInfo*;

class PackageInfo;
using PackageInfoRef = PackageInfo*;

class ParameterInfo;
using ParameterInfoRef = ParameterInfo*;

using FuncPtr = void(*)(void*);

// at first glance, there is no need to expose this type or at least RAW_POINTER_OBJECT.
// however in consideration that there are lots of differences for runtime apis to support different gc,
// this is acceptable.
enum class AllocType {
    MOVEABLE_OBJECT = 0,
    PINNED_OBJECT,
    RAW_POINTER_OBJECT,
};

#ifdef __cplusplus
extern "C" {
#endif
/* Get the aligned value. */
#define MRT_ALIGN(x, a) (((x) + (a)-1) & ~((a)-1))
#ifdef __cplusplus
}
#endif
} // namespace MapleRuntime

#endif // MRT_TYPE_DEF_H
