// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include <cstddef>

namespace {
// TypeInfo is 96 bytes on x86_64 today. The harness checks the product size
// before treating this storage as a TypeInfo, so a layout change fails closed.
alignas(8) unsigned char typeInfoStorage[96];
}

extern "C" void* AbastressTypeInfoAddress()
{
    return typeInfoStorage;
}

extern "C" size_t AbastressTypeInfoStorageSize()
{
    return sizeof(typeInfoStorage);
}
