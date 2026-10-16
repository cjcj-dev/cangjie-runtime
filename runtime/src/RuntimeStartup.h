#ifndef MRT_RUNTIME_STARTUP_H
#define MRT_RUNTIME_STARTUP_H

#include "schedule.h"

extern "C" __attribute__((visibility("hidden"))) void NotifyRuntimeSchedulerReady(ScheduleHandle scheduler);

#endif
