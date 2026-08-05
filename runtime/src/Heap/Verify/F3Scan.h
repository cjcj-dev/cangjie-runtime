// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_HEAP_F3_SCAN_H
#define MRT_HEAP_F3_SCAN_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {

class BaseObject;
template <bool isVolatile>
class RefField;

// F3 scan forensics for bucket-2 (holder marked, target dead):
//   MRT_GCV2_F3_SCAN=1       dump type/gctib/enum at F3_DEATH abort (default off)
//   MRT_GCV2_F3S_POSCTRL=1   skip one major edge at a precise field offset (default off)
//   MRT_GCV2_F3S_POSCTRL_OFF=<bytes>  field offset from object base (default 40)
//   MRT_GCV2_F3S_POSCTRL_TYPE=<substr> optional type-name filter (empty = any)
// Does not widen IsValidObject / IsMarked / TryUntag; evidence only.
class F3Scan {
public:
    static bool Enabled();
    static bool PosCtrlEnabled();

    // True if this edge should be intentionally not followed (bucket-2 POSCTRL).
    static bool ShouldSkipEdge(BaseObject* holder, RefField<>* field, BaseObject* target, const char* site);

    // At F3 abort: dump holder type, size, slot offset, gctib bit, simulated enum set,
    // and classify S1/S2/S3 into verdictBuf.
    static void DumpAtAbort(BaseObject* holder, RefField<>* field, BaseObject* target, char* verdictBuf,
                            size_t verdictBufSize);

    static bool PosCtrlMatch(BaseObject* holder, BaseObject* target);
};

} // namespace MapleRuntime

#endif // MRT_HEAP_F3_SCAN_H
