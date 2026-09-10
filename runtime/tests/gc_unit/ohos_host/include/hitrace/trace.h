// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_GC_UNIT_OHOS_HOST_HITRACE_TRACE_H
#define MRT_GC_UNIT_OHOS_HOST_HITRACE_TRACE_H

#include <stdint.h>

static inline void OH_HiTrace_StartTrace(const char*) {}
static inline void OH_HiTrace_FinishTrace() {}
static inline void OH_HiTrace_StartAsyncTrace(const char*, int32_t) {}
static inline void OH_HiTrace_FinishAsyncTrace(const char*, int32_t) {}
static inline void OH_HiTrace_CountTrace(const char*, int64_t) {}

#endif
