#include "inner/schedule_impl.h"
#include <cstddef>
#include <cstdio>

int main()
{
    std::printf("{\"num\":%zu,\"mutex\":%zu,\"runq\":%zu,\"owner\":%zu}\n",
        offsetof(Schedule, schdCJThread) + offsetof(ScheduleCJThread, num),
        offsetof(Schedule, schdCJThread) + offsetof(ScheduleCJThread, mutex),
        offsetof(Schedule, schdCJThread) + offsetof(ScheduleCJThread, runq),
        offsetof(pthread_mutex_t, __data.__owner));
}
