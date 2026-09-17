#pragma once
#include <cstdarg>
#include <cstddef>
namespace MapleRuntime {
class ZBarrierSet;
class ZInitializer {
public:
    explicit ZInitializer(ZBarrierSet* barrier_set);
};
class ZInitialize {
public:
    static constexpr size_t ErrorMessageLength = 256;
    static void initialize();
    static void initialize(ZBarrierSet* barrier_set);
    static void register_error(bool debug, const char* error_msg);
    static void error(const char* msg_format, ...);
    static void finish();
    static bool had_error();
private:
    static char error_message[ErrorMessageLength];
    static bool had_error_flag;
    static bool finished;
};
}
