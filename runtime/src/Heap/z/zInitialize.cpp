#include "Heap/z/zInitialize.hpp"
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
#include "Heap/z/zBarrierSet.hpp"
namespace MapleRuntime {
char ZInitialize::_error_message[ErrorMessageLength] = {};
bool ZInitialize::had_error_flag = false;
bool ZInitialize::finished = false;

ZInitializer::ZInitializer(ZBarrierSet* barrier_set) { ZInitialize::initialize(barrier_set); }

void ZInitialize::initialize(ZBarrierSet* barrier_set)
{
    ZGlobalsPointers::initialize();
    ZCPU::initialize();
    ZStatValue::initialize();
    ZThreadLocalAllocBuffer::initialize();
    ZLargePages::initialize();
    BarrierSet::set_barrier_set(barrier_set);
    ZJNICritical::initialize();
    ZDriver::initialize();
}

void ZInitialize::register_error(bool debug, const char* error_msg)
{
    CHECK_DETAIL(!finished, "Only register errors during initialization");
    if (!had_error_flag) {
        std::strncpy(_error_message, error_msg, ErrorMessageLength - 1);
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

void ZInitialize::finish()
{
    CHECK_DETAIL(!finished, "Only finish initialization once");
    finished = true;
}

const char* ZInitialize::error_message()
{
    MRT_ASSERT(had_error(), "Should have registered an error");
    if (had_error()) {
        return _error_message;
    }
    return "Unknown error, check error GC logs";
}

bool ZInitialize::had_error() { return had_error_flag; }
}
