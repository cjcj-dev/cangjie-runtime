// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#ifndef MRT_PACKAGE_INIT_TEST_H
#define MRT_PACKAGE_INIT_TEST_H
#include <stdint.h>
#include <stdbool.h>
#ifdef MRT_TESTABLE_INTERNALS
#ifdef __cplusplus
extern "C" {
#define PACKAGE_INIT_TEST_NOEXCEPT noexcept
#else
#define PACKAGE_INIT_TEST_NOEXCEPT
#endif
// Arm before starting the initializer. Exact code identities and phase are
// required; an unrelated package/unit never consumes the pause. One-shot.
bool MRT_PackageInitArmCompletePause(const void* package, const void* unit, uint32_t phase)
    PACKAGE_INIT_TEST_NOEXCEPT;
// True means Complete reached the pre-publication wait point. The wait uses
// the current logical CJThread, outside the state graph and loader locks.
bool MRT_PackageInitCompletePauseReached(void) PACKAGE_INIT_TEST_NOEXCEPT;
void MRT_PackageInitReleaseCompletePause(void) PACKAGE_INIT_TEST_NOEXCEPT;
#undef PACKAGE_INIT_TEST_NOEXCEPT
#ifdef __cplusplus
}
#endif
#endif
#endif
