// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/VerifyOption.h"

#include <cstdlib>
#include <cstring>

namespace MapleRuntime {

const char* VerifyMarkSourceName(VerifyMarkSource source)
{
    switch (source) {
        case VerifyMarkSource::IndependentVsBitmap:
            return "independent-vs-bitmap";
        case VerifyMarkSource::MinorClosure:
            return "minor-closure";
        case VerifyMarkSource::RegionMarkBitmap:
            return "region-bitmap";
        case VerifyMarkSource::IndependentRetrace:
            return "independent-retrace";
        default:
            return "unknown";
    }
}

VerifyMarkSource ParseVerifyMarkSource()
{
    const char* v = std::getenv("MRT_GCV2_VERIFY_MARK_SOURCE");
    if (v == nullptr || v[0] == '\0' || std::strcmp(v, "default") == 0 ||
        std::strcmp(v, "independent-vs-bitmap") == 0 || std::strcmp(v, "independent") == 0) {
        return VerifyMarkSource::IndependentVsBitmap;
    }
    if (std::strcmp(v, "minor-closure") == 0 || std::strcmp(v, "minor") == 0) {
        return VerifyMarkSource::MinorClosure;
    }
    if (std::strcmp(v, "region-bitmap") == 0 || std::strcmp(v, "bitmap") == 0) {
        return VerifyMarkSource::RegionMarkBitmap;
    }
    if (std::strcmp(v, "independent-retrace") == 0 || std::strcmp(v, "retrace") == 0) {
        return VerifyMarkSource::IndependentRetrace;
    }
    return VerifyMarkSource::IndependentVsBitmap;
}

} // namespace MapleRuntime
