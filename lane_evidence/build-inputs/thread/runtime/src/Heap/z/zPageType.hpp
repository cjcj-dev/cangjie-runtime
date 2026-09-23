// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_Z_PAGE_TYPE_H
#define MRT_Z_PAGE_TYPE_H

#include <cstdint>

namespace MapleRuntime {

// zPageType.hpp:29-33
enum class ZPageType : uint8_t {
    small,
    medium,
    large
};

} // namespace MapleRuntime

#endif // MRT_Z_PAGE_TYPE_H
