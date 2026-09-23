#include "Heap/z/zInitialize.hpp"
#include "Mutator/ThreadLocal.h"
#include "Heap/z/zAddress.hpp"
#include "Heap/z/zCPU.hpp"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zJNICritical.hpp"
#include "Heap/z/zLargePages.hpp"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zThreadLocalAllocBuffer.hpp"
#include "Base/Log.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
namespace MapleRuntime {
char ZInitialize::error_message[ErrorMessageLength] = {};
bool ZInitialize::had_error_flag = false;
bool ZInitialize::finished = false;

ZInitializer::ZInitializer(ZBarrierSet* barrier_set) { ZInitialize::initialize(barrier_set); }

void ZInitialize::initialize() { initialize(nullptr); }

void ZInitialize::initialize(ZBarrierSet*)
{
    ZGlobalsPointers::initialize();
    ThreadLocal::InitializeCleaner();
    ZCPU::initialize();
    ZStatValue::initialize();
    ZThreadLocalAllocBuffer::initialize();
    ZLargePages::initialize();
    ZJNICritical::initialize();
    ZDriver::initialize();
}

void ZInitialize::register_error(bool debug, const char* error_msg)
{
    if (finished) {
        return;
    }
    if (!had_error_flag) {
        std::strncpy(error_message, error_msg, ErrorMessageLength - 1);
        had_error_flag = true;
    }
    (void)debug;
    LOG(RTLOG_ERROR, "%s", error_msg);
}

void ZInitialize::error(const char* msg_format, ...)
{
    char buf[ErrorMessageLength];
    va_list args;
    va_start(args, msg_format);
    std::vsnprintf(buf, sizeof(buf), msg_format, args);
    va_end(args);
    register_error(false, buf);
}

void ZInitialize::finish() { finished = true; }

bool ZInitialize::had_error() { return had_error_flag; }
}
