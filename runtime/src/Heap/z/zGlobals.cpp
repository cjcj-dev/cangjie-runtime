// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "Heap/z/zGlobals.hpp"
#include "Heap/z/z_globals.hpp"
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
namespace MapleRuntime {
// gc/shared/gc_globals.hpp ConcGCThreads; zArguments.cpp:67-81 sets it before the
// heap comes up. ZCollectedHeap::start_gc_threads publishes the concurrent budget
// after heap initialization; this initial capacity covers early per-worker storage.
bool UseDynamicNumberOfGCThreads = true;
double ZCollectionIntervalMinor = -1.0;
double ZCollectionIntervalMajor = -1.0;
// gc_globals.hpp / z_globals.hpp defaults; ZArguments applies flag origins.
std::atomic<size_t> SoftMaxHeapSize{0};
uint32_t MaxTenuringThreshold = 15;
int32_t ZTenuringThreshold = -1;
uint32_t ConcGCThreads = 64;
uint32_t ZYoungGCThreads = 64;
uint32_t ZOldGCThreads = 64;
}
