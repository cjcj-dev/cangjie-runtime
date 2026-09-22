// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "Cangjie.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <string>
#include <unistd.h>

extern "C" void CJ_MRT_CjRuntimeInit();

int main(int argc, char** argv)
{
    if (argc != 5) return 2;
    if (std::strcmp(argv[1], "env") == 0) {
        // The same exported initialization entry used by compiled Cangjie programs.
        CJ_MRT_CjRuntimeInit();
    } else {
        RuntimeParam param {};
        param.heapParam.heapSize = 256 * 1024;
        param.coParam.processorNum = 1;
        param.gcParam.concGCThreads = std::strtoul(argv[2], nullptr, 10);
        param.gcParam.youngGCThreads = std::strtoul(argv[3], nullptr, 10);
        param.gcParam.oldGCThreads = std::strtoul(argv[4], nullptr, 10);
        param.gcParam.staticGCThreads = std::strcmp(argv[1], "static") == 0;
        const int rc = InitCJRuntime(&param);
        std::printf("INIT_RC=%d\n", rc);
        if (rc != 0) return 3;
    }
    DIR* tasks = opendir("/proc/self/task");
    if (tasks == nullptr) return 4;
    unsigned young = 0, old = 0;
    while (dirent* entry = readdir(tasks)) {
        std::ifstream comm(std::string("/proc/self/task/") + entry->d_name + "/comm");
        std::string name;
        std::getline(comm, name);
        if (name.rfind("ZWorkerYoung#", 0) == 0) ++young;
        if (name.rfind("ZWorkerOld#", 0) == 0) ++old;
        if (name.rfind("ZWorker", 0) == 0) std::printf("TASK %s %s\n", entry->d_name, name.c_str());
    }
    closedir(tasks);
    std::printf("OBSERVED young=%u old=%u pid=%d\n", young, old, getpid());
    std::fflush(nullptr);
    // Initialization is the behavior under test; no GC load is needed.
    std::_Exit(0);
}
