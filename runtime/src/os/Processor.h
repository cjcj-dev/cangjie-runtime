// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#pragma once
#include <cstdint>
namespace MapleRuntime {
namespace OS {
// HotSpot runtime/os.cpp:474,1944-1947: captured at platform initialization.
void InitializeProcessorCount();
uint32_t InitialActiveProcessorCount();
}
}
