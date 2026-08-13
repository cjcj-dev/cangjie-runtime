// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_START_WHO_DIAG_H
#define MRT_START_WHO_DIAG_H

#include <cstdint>

namespace MapleRuntime {
class BaseObject;

// startwho: caller-side breadcrumb around BaseObject::GetSize.
// Gate: MRT_GCV2_STARTWHO=1 or MRT_GCV2_DIAG token startwho. Default off.
namespace StartWhoDiag {

bool Enabled();

class ScopedCaller {
public:
    ScopedCaller(const char* caller, BaseObject* object);
    ~ScopedCaller();

    ScopedCaller(const ScopedCaller&) = delete;
    ScopedCaller& operator=(const ScopedCaller&) = delete;

private:
    bool active_;
    const char* previousCaller_;
    uintptr_t previousObject_;
};

void NoteCrash();

void Report(const char* point);

} // namespace StartWhoDiag
} // namespace MapleRuntime

#endif // MRT_START_WHO_DIAG_H
