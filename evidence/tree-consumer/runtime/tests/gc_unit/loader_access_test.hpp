// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#pragma once
#include "Loader/CjFileLoader/CjFileLoader.h"
#include "Loader/ElfUnloadQuiescence.h"
#include "os/Path.h"
#include <algorithm>
namespace MapleRuntime {
class CJFileLoaderTest {
public:
    static void* Handle(const CJFileLoader& loader, const char* name) {
        const CString base = Os::Path::GetBaseName(name);
        std::lock_guard<std::mutex> lock(loader.libCjsoHandlersMutex);
        const auto it = std::find_if(loader.cjLibHandlers.begin(), loader.cjLibHandlers.end(),
            [&](const auto& item) { return base == Os::Path::GetBaseName(item.baseName.Str()); });
        return it == loader.cjLibHandlers.end() ? nullptr : it->handler;
    }
};
class ElfUnloadQuiescenceTest {
public:
    static bool UnloadPending() {
        return (ElfUnloadQuiescence::State().load(std::memory_order_acquire) & ElfUnloadQuiescence::WRITER_BIT) != 0;
    }
    static bool TrySharedAdmission() {
        auto& mutex = ElfUnloadQuiescence::TaskAdmissionMutex();
        if (!mutex.try_lock_shared()) { return false; }
        mutex.unlock_shared();
        return true;
    }
};
}
