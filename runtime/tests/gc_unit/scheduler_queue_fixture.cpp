#include "Cangjie.h"
#include <array>
#include <cstdio>

extern "C" int CJ_CJThreadResched(void);

static void* RescheduleTask(void* argument)
{
    CJ_CJThreadResched();
    CJ_CJThreadResched();
    return argument;
}

int main()
{
    RuntimeParam parameters{};
    parameters.coParam.processorNum = 2;
    if (InitCJRuntime(&parameters) != E_OK) {
        return 2;
    }
    std::array<CJThreadHandle, 16> tasks{};
    std::array<unsigned int, 16> arguments{};
    for (unsigned int index = 0; index < tasks.size(); ++index) {
        arguments[index] = index;
        tasks[index] = RunCJTask(RescheduleTask, &arguments[index]);
        if (!tasks[index]) {
            return 3;
        }
    }
    for (unsigned int index = 0; index < tasks.size(); ++index) {
        void* result = nullptr;
        int status = GetTaskRet(tasks[index], &result);
        std::printf("SCHEDULER_TASK_TARGET index=%u status=%d result_matches=%d\n",
                    index, status, result == &arguments[index]);
        if (status != E_OK || result != &arguments[index]) {
            return 1;
        }
        ReleaseHandle(tasks[index]);
    }
    return FiniCJRuntime() == E_OK ? 0 : 4;
}
