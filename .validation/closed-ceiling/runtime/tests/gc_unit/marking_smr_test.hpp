// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#pragma once
#include "Heap/z/zMarkingSMR.hpp"
namespace MapleRuntime {
class MarkingSMRTest {
public:
    static void reclaim(MarkingSMR& smr) { smr.reclaim(); }
    static size_t pending_count(const MarkingSMR& smr) { return smr.pending_count(); }
};
}
