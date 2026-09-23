// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_GC_UNIT_OHOS_HOST_DLFCN_H
#define MRT_GC_UNIT_OHOS_HOST_DLFCN_H

#include_next <dlfcn.h>

// OHOS bionic exposes this namespace descriptor. The host arm needs its
// layout only to compile LoaderManager; the tests never claim namespace-loader
// fidelity.
struct Dl_namespace {
    char name[256];
};

#endif
