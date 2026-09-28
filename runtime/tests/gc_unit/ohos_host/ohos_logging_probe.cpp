// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Base/Log.h"

// Link to the OHOS-host SO: never compile Log.cpp into this executable.
int main(int argc, char** argv)
{
    if (argc != 2) {
        return 2;
    }
    if (std::strcmp(argv[1], "shim") == 0) {
        OH_LOG_DEBUG(LOG_APP, "plain=%d", 42);
        OH_LOG_INFO(LOG_APP, "public=%{public}s", "visible");
        OH_LOG_WARN(LOG_APP, "private=%{private}d", 17);
        OH_LOG_ERROR(LOG_APP, "width=%{public}*.*f", 6, 2, 1.25);
        OH_LOG_FATAL(LOG_APP, "literal=%%{public}s percent=%% value=%{public}d", 23);
        return 0; // hilog emits; the runtime owns fatal termination.
    }
    const bool fatal = std::strcmp(argv[1], "fatal") == 0;
    if (!fatal && std::strcmp(argv[1], "info") != 0) {
        return 2;
    }
    std::puts("OHOS_LOGGING_ENTRY Logger::FormatLog");
    std::fflush(stdout);
    MapleRuntime::Logger::GetLogger().FormatLog(fatal ? RTLOG_FATAL : RTLOG_INFO, true,
                                               "ohos-host diagnostic value=%d text=%s", 1244, "visible");
    return 0;
}
