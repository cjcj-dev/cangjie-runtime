// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.


#pragma once


namespace MapleRuntime {
#if defined(MRT_TESTABLE_INTERNALS)
inline uintptr_t StoreBarrierBuffer::LastProcessedColorForTest() const { return lastProcessedColor; }
#endif
} // namespace MapleRuntime
