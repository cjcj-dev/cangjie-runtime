// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Heap/Verify/VerifyOption.h"

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

} // namespace MapleRuntime
