// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/F3Scan.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Common/BaseObject.h"
#include "ObjectModel/MArray.inline.h"
#include "ObjectModel/MClass.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
namespace {

constexpr size_t kMaxEnumSlots = 128;
constexpr size_t kMaxBitmapDump = 64;

std::atomic<int> gPosFired{ 0 };
void* gPosHolder = nullptr;
void* gPosField = nullptr;
void* gPosTarget = nullptr;
intptr_t gPosOff = -1;

bool EnvIsOne(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "1") == 0;
}

intptr_t EnvIptr(const char* name, intptr_t def)
{
    const char* v = std::getenv(name);
    if (v == nullptr || v[0] == '\0') {
        return def;
    }
    char* end = nullptr;
    long long x = std::strtoll(v, &end, 0);
    if (end == v) {
        return def;
    }
    return static_cast<intptr_t>(x);
}

// Simulate TraceObjectRefFields enumeration into offs[] (object-base offsets).
// Mirrors WCollector::TraceObjectRefFields (WCollector.cpp:843-881).
size_t EnumRefOffsets(BaseObject* obj, intptr_t* offs, size_t cap)
{
    size_t n = 0;
    if (obj == nullptr || !obj->IsValidObject()) {
        return 0;
    }
    TypeInfo* typeInfo = obj->GetTypeInfo();
    if (typeInfo == nullptr || !typeInfo->HasRefField()) {
        return 0;
    }
    auto push = [&](RefField<>& field) {
        if (n >= cap) {
            return;
        }
        offs[n++] = BaseObject::FieldOffset(obj, &field);
    };
    if (UNLIKELY(typeInfo->IsRawArray())) {
        MArray* array = reinterpret_cast<MArray*>(obj);
        MIndex arrayLength = array->GetLength();
        TypeInfo* componentTypeInfo = array->GetComponentTypeInfo();
        if (componentTypeInfo != nullptr && componentTypeInfo->IsStructType()) {
            GCTib gcTib = componentTypeInfo->GetGCTib();
            MAddress contentAddr = reinterpret_cast<Uptr>(array) + MArray::GetContentOffset();
            size_t elementSize = array->GetElementSize();
            for (MIndex i = 0; i < arrayLength; ++i) {
                gcTib.ForEachBitmapWord(contentAddr, push);
                contentAddr += elementSize;
            }
        } else if (componentTypeInfo != nullptr &&
                   (componentTypeInfo->IsObjectType() || componentTypeInfo->IsArrayType() ||
                    componentTypeInfo->IsInterface())) {
            RefField<>* arrayContent = reinterpret_cast<RefField<>*>(array->ConvertToCArray());
            for (MIndex i = 0; i < arrayLength; ++i) {
                push(arrayContent[i]);
            }
        }
        return n;
    }
    MAddress contentAddr = reinterpret_cast<MAddress>(obj) + TYPEINFO_PTR_SIZE;
    obj->GetGCTib().ForEachBitmapWord(contentAddr, push);
    return n;
}

// Pure gctib bitmap walk for non-array objects (content starts at +TYPEINFO_PTR_SIZE).
// For arrays, returns same as EnumRefOffsets (full walk).
size_t GctibRefOffsets(BaseObject* obj, intptr_t* offs, size_t cap)
{
    return EnumRefOffsets(obj, offs, cap);
}

bool OffInSet(const intptr_t* offs, size_t n, intptr_t off)
{
    for (size_t i = 0; i < n; ++i) {
        if (offs[i] == off) {
            return true;
        }
    }
    return false;
}

void DumpBitmapHex(BaseObject* obj, char* buf, size_t bufSize)
{
    if (buf == nullptr || bufSize == 0) {
        return;
    }
    buf[0] = '\0';
    if (obj == nullptr || !obj->IsValidObject()) {
        std::snprintf(buf, bufSize, "NA");
        return;
    }
    TypeInfo* ti = obj->GetTypeInfo();
    if (ti == nullptr) {
        std::snprintf(buf, bufSize, "null_ti");
        return;
    }
    GCTib g = ti->GetGCTib();
    if (g.IsGCTibWord()) {
        std::snprintf(buf, bufSize, "short=0x%llx",
                      static_cast<unsigned long long>(g.bitmap.bitmap & ~SIGN_BIT));
        return;
    }
    if (g.gctib == nullptr) {
        std::snprintf(buf, bufSize, "std=null");
        return;
    }
    size_t n = g.gctib->nBitmapWords;
    if (n > kMaxBitmapDump) {
        n = kMaxBitmapDump;
    }
    size_t pos = 0;
    int w = std::snprintf(buf + pos, bufSize - pos, "std_n=%u bytes=", g.gctib->nBitmapWords);
    if (w < 0 || static_cast<size_t>(w) >= bufSize - pos) {
        return;
    }
    pos += static_cast<size_t>(w);
    for (size_t i = 0; i < n && pos + 3 < bufSize; ++i) {
        w = std::snprintf(buf + pos, bufSize - pos, "%02x", g.gctib->bitmapWords[i]);
        if (w < 0 || static_cast<size_t>(w) >= bufSize - pos) {
            break;
        }
        pos += static_cast<size_t>(w);
    }
}

void DumpOffList(const intptr_t* offs, size_t n, char* buf, size_t bufSize)
{
    if (buf == nullptr || bufSize == 0) {
        return;
    }
    size_t pos = 0;
    buf[0] = '\0';
    for (size_t i = 0; i < n && pos + 8 < bufSize; ++i) {
        int w = std::snprintf(buf + pos, bufSize - pos, "%s%zd", i == 0 ? "" : ",", offs[i]);
        if (w < 0 || static_cast<size_t>(w) >= bufSize - pos) {
            break;
        }
        pos += static_cast<size_t>(w);
    }
    if (n == 0 && bufSize > 0) {
        std::snprintf(buf, bufSize, "empty");
    }
}

} // namespace

bool F3Scan::Enabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_F3_SCAN") || EnvIsOne("MRT_GCV2_F3S_POSCTRL");
    return on;
}

bool F3Scan::PosCtrlEnabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_F3S_POSCTRL");
    return on;
}

bool F3Scan::ShouldSkipEdge(BaseObject* holder, void* field, BaseObject* target, const char* site)
{
    if (!PosCtrlEnabled() || holder == nullptr || field == nullptr || target == nullptr) {
        return false;
    }
    if (site == nullptr || std::strncmp(site, "TraceRefField", 13) != 0) {
        return false;
    }
    if (gPosFired.load(std::memory_order_acquire) != 0) {
        return false;
    }
    if (!holder->IsValidObject()) {
        return false;
    }
    static const intptr_t wantOff = EnvIptr("MRT_GCV2_F3S_POSCTRL_OFF", 40);
    static const char* wantType = std::getenv("MRT_GCV2_F3S_POSCTRL_TYPE");
    const intptr_t off = BaseObject::FieldOffset(holder, field);
    if (off != wantOff) {
        return false;
    }
    TypeInfo* ti = holder->GetTypeInfo();
    const char* name = (ti != nullptr) ? ti->GetName() : nullptr;
    if (wantType != nullptr && wantType[0] != '\0') {
        if (name == nullptr || std::strstr(name, wantType) == nullptr) {
            return false;
        }
    }
    int expected = 0;
    if (!gPosFired.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        return false;
    }
    gPosHolder = holder;
    gPosField = field;
    gPosTarget = target;
    gPosOff = off;
    VLOG(REPORT,
         "[GCV2][F3S][POSCTRL] skip_edge holder=%p field=%p target=%p off=%zd type=%s size=%zu "
         "site=%s (precise off POSCTRL for bucket2 closed loop)",
         holder, field, target, off, name != nullptr ? name : "?", holder->GetSize(),
         site != nullptr ? site : "?");
    return true;
}

bool F3Scan::PosCtrlMatch(BaseObject* holder, BaseObject* target)
{
    return gPosFired.load(std::memory_order_acquire) != 0 && holder == gPosHolder && target == gPosTarget;
}

void F3Scan::DumpAtAbort(BaseObject* holder, void* field, BaseObject* target, char* verdictBuf, size_t verdictBufSize)
{
    if (verdictBuf != nullptr && verdictBufSize > 0) {
        verdictBuf[0] = '\0';
    }
    if (!Enabled() && !EnvIsOne("MRT_GCV2_F3_DEATH")) {
        if (verdictBuf != nullptr && verdictBufSize > 0) {
            std::snprintf(verdictBuf, verdictBufSize, "F3S_OFF");
        }
        return;
    }
    // Always dump under F3_DEATH when called from abort path; Enabled() gates hot POSCTRL.
    if (holder == nullptr || field == nullptr) {
        if (verdictBuf != nullptr && verdictBufSize > 0) {
            std::snprintf(verdictBuf, verdictBufSize, "F3S_NULL");
        }
        return;
    }

    const intptr_t slotOff = BaseObject::FieldOffset(holder, field);
    const int holderValid = holder->IsValidObject() ? 1 : 0;
    TypeInfo* ti = holderValid ? holder->GetTypeInfo() : nullptr;
    const char* typeName = (ti != nullptr) ? ti->GetName() : "INVALID";
    const size_t objSize = holderValid ? holder->GetSize() : 0;
    const int hasRef = (ti != nullptr && ti->HasRefField()) ? 1 : 0;
    const int isWeak = (ti != nullptr && ti->IsWeakRefType()) ? 1 : 0;
    const int isRaw = (ti != nullptr && ti->IsRawArray()) ? 1 : 0;
    const int isClass = (ti != nullptr && ti->IsClass()) ? 1 : 0;
    const unsigned typeKind = (ti != nullptr) ? static_cast<unsigned>(ti->GetType()) : 0u;
    const size_t instSize = (ti != nullptr) ? static_cast<size_t>(ti->GetInstanceSize()) : 0;

    intptr_t enumOffs[kMaxEnumSlots];
    const size_t enumN = holderValid ? EnumRefOffsets(holder, enumOffs, kMaxEnumSlots) : 0;
    const int inEnum = OffInSet(enumOffs, enumN, slotOff) ? 1 : 0;

    char bmpBuf[256];
    DumpBitmapHex(holder, bmpBuf, sizeof(bmpBuf));
    char enumBuf[512];
    DumpOffList(enumOffs, enumN, enumBuf, sizeof(enumBuf));

    // S1: slot not in gctib/enum set (bitmap missing)
    // S2: holder is weakref type (weak path skips strong TraceObjectRefFields on holder)
    // S3: hasRef but enum empty or truncated vs expected, or raw-array branch anomaly
    //     (slot in theory should be walked but simulation also misses → enum path bug;
    //      if inEnum=1 then Trace path would have visited → not S1/S3 at enum layer)
    const char* sClass = "F3S_NONE_OF_THREE";
    if (isWeak) {
        sClass = "F3S_S2_WEAK_IsWeakRef_skip_holder_fields";
    } else if (!hasRef) {
        sClass = "F3S_S1_HasRefField_false";
    } else if (inEnum == 0) {
        sClass = isRaw ? "F3S_S3_ENUM_rawarray_miss" : "F3S_S1_GCTIB_MISSING_slot";
    } else {
        // Slot is in enum set ⇒ TRACE would call TraceRefField on it.
        // If still bucket2, leak is not gctib/enum (SATB/post-enum rewrite or other).
        sClass = "F3S_IN_ENUM_not_S1S2S3_enum_would_visit";
    }

    const int posMatch = PosCtrlMatch(holder, target) ? 1 : 0;

    VLOG(REPORT,
         "[GCV2][F3S] holder=%p field=%p target=%p slotOff=%zd type=%s typeKind=%u objSize=%zu "
         "instSize=%zu hasRef=%d isWeak=%d isRaw=%d isClass=%d inEnum=%d enumN=%zu "
         "posMatch=%d posOff=%zd posFired=%d gctib={%s} enumOffs={%s} class=%s",
         holder, field, target, slotOff, typeName != nullptr ? typeName : "?", typeKind, objSize, instSize, hasRef,
         isWeak, isRaw, isClass, inEnum, enumN, posMatch, gPosOff,
         gPosFired.load(std::memory_order_acquire), bmpBuf, enumBuf, sClass);

    if (verdictBuf != nullptr && verdictBufSize > 0) {
        std::snprintf(verdictBuf, verdictBufSize, "%s_off%zd_in%d_weak%d_raw%d_pos%d_type_%s", sClass, slotOff, inEnum,
                      isWeak, isRaw, posMatch, typeName != nullptr ? typeName : "?");
    }
}

} // namespace MapleRuntime
