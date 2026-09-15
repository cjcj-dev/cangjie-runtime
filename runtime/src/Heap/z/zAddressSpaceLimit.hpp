// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#pragma once
#include <cstddef>
namespace MapleRuntime {
class AddressSpaceBudget {
public:
    static AddressSpaceBudget Seal(size_t availableBytes, size_t safeFraction = 2);
    static AddressSpaceBudget SealProcessBudget();

    bool IsSealed() const { return sealed; }
    bool Allows(size_t bytes) const { return sealed && bytes <= safeBytes; }
    size_t AvailableBytes() const { return availableBytes; }
    size_t SafeBytes() const { return safeBytes; }

private:
    size_t availableBytes{ 0 };
    size_t safeBytes{ 0 };
    bool sealed{ false };
};

}
