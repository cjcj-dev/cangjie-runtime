// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "Cangjie.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
extern "C" void CJ_MRT_CjRuntimeInit();
int main(int argc, char** argv)
{
    if (argc != 3) return 2;
    if (std::strcmp(argv[1], "env") == 0) {
        CJ_MRT_CjRuntimeInit();
    } else {
        RuntimeParam param{};
        param.heapParam.heapSize = 64 * 1024;
        param.coParam.processorNum = 1;
        param.gcParam.concGCThreads = 2;
        param.gcParam.youngGCThreads = 2;
        param.gcParam.oldGCThreads = 2;
        param.gcParam.backupGCInterval = std::strtoull(argv[2], nullptr, 10);
        const int rc = InitCJRuntime(&param);
        std::printf("INIT_RC=%d\n", rc);
        if (rc != 0) return 3;
    }
    // The debugger observes the real director thread while this process stays alive.
    sleep(300);
    std::fflush(nullptr);
    std::_Exit(0);
}
