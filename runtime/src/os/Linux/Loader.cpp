// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
#include "os/Loader.h"

#include <dlfcn.h>
#if defined(__GLIBC__) && !defined(__OHOS__) && !defined(__ANDROID__)
#include <link.h>
#endif

#include "Base/Log.h"
namespace MapleRuntime {
namespace Os {
void* Loader::LoadBinaryFile(const char* path)
{
    if (path == nullptr) {
        return nullptr;
    }
#if defined(__GLIBC__) && !defined(__OHOS__) && !defined(__ANDROID__)
    // Anchor the lookup in this runtime image. A NULL dlopen handle can name
    // the base executable even when this function runs in another namespace.
    static const char namespaceAnchor = 0;
    Dl_info selfInfo {};
    void* selfMap = nullptr;
    if (dladdr1(&namespaceAnchor, &selfInfo, &selfMap, RTLD_DL_LINKMAP) == 0 || selfMap == nullptr) {
        LOG(RTLOG_ERROR, "runtime namespace image lookup failed");
        return nullptr;
    }
    const auto* expected = static_cast<const link_map*>(selfMap);
    void* self = dlopen(expected->l_name[0] == '\0' ? nullptr : expected->l_name, RTLD_NOW | RTLD_NOLOAD);
    if (self == nullptr) {
        LOG(RTLOG_ERROR, "runtime namespace handle lookup failed");
        return nullptr;
    }
    link_map* actual = nullptr;
    Lmid_t namespaceId = LM_ID_BASE;
    const bool identified = dlinfo(self, RTLD_DI_LINKMAP, &actual) == 0 && actual == expected &&
        dlinfo(self, RTLD_DI_LMID, &namespaceId) == 0;
    const int closed = dlclose(self);
    if (!identified || closed != 0) {
        LOG(RTLOG_ERROR, "runtime namespace identity lookup failed");
        return nullptr;
    }
    // Only base supports GLOBAL. Explicit dependencies and handle-based symbol
    // lookup remain available for LOCAL images in the verified caller namespace.
    void* handler = namespaceId == LM_ID_BASE ? dlopen(path, RTLD_NOW | RTLD_GLOBAL) :
        dlmopen(namespaceId, path, RTLD_NOW | RTLD_LOCAL);
#else
    void* handler = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
#endif
    if (handler == nullptr) {
        LOG(RTLOG_ERROR, dlerror());
    }
    return handler;
}

int Loader::UnloadBinaryFile(void* handler)
{
    if (handler == nullptr) {
        return -1;
    }
    int ret = dlclose(handler);
    return ret;
}

void* Loader::FindSymbol(void* handler, const char* symbolName)
{
    if (symbolName == nullptr) {
        return nullptr;
    }
    // handler can be nullptr
    return dlsym(handler, symbolName);
}

int Loader::GetBinaryInfoFromAddress(const void* address, BinaryInfo* binInfo)
{
    if (address == nullptr) {
        return 0;
    }
    Dl_info dlInfo;
    int ret = dladdr(address, &dlInfo);
    if (!ret) {
        return 0;
    }
    binInfo->filePathName = const_cast<char*>(dlInfo.dli_fname);
    binInfo->symbolName = const_cast<char*>(dlInfo.dli_sname);
    return ret;
}
} // namespace Os
} // namespace MapleRuntime
