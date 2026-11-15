// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#ifndef MRT_NATIVE_IMAGE_RETENTION_H
#define MRT_NATIVE_IMAGE_RETENTION_H

namespace MapleRuntime {
// Paired with Cjstart's protected __CJ_ImageRetainV1. See
// docs/image-retention-v1.md for the native ABI and old-image boundary.
using NativeImageRetainerV1 = void* (*)(const char*, int);
constexpr const char* NATIVE_IMAGE_RETAINER_SYMBOL_V1 = "__CJ_ImageRetainV1";
} // namespace MapleRuntime
#endif
