// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "Heap/z/zGlobals.hpp"
namespace MapleRuntime {
// ZGC zGlobals.cpp:26-37. Page-size policy initializes the medium tier;
// pointer-sized object alignment is the runtime ABI's fixed minimum.
int ZPageSizeMediumMaxShift;
size_t ZPageSizeMediumMax;
size_t ZPageSizeMediumMin;
bool ZPageSizeMediumEnabled;
size_t ZObjectSizeLimitMedium;
namespace {
const int MinObjAlignmentInBytes = 8;
const int LogMinObjAlignmentInBytes = 3;
}
const int& ZObjectAlignmentSmallShift = LogMinObjAlignmentInBytes;
int ZObjectAlignmentMediumShift;
const int& ZObjectAlignmentSmall = MinObjAlignmentInBytes;
int ZObjectAlignmentMedium;
}
