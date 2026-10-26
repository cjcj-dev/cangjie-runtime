// Real C2N -> native C++ -> N2C callback consumer. Build as a Windows DLL
// alongside windows_exception_transfer.cj using the qualified target tuple.
#include <cstdint>

static uint32_t cleanupCount = 0;
static uint32_t returnPhase = 0;

struct NativeResource {
    ~NativeResource() { ++cleanupCount; }
};

__attribute__((noinline)) static void InvokeCallback(void (*callback)())
{
    NativeResource inner;
    callback();
    // An uncaught managed callback returns through N2C with pending state.
    // C++ cleanup must still run before the outer C2N transfers to its catch.
    returnPhase = 1;
}

extern "C" __declspec(dllexport) void WindowsCrossPending(void (*callback)())
{
    cleanupCount = 0;
    returnPhase = 0;
    NativeResource outer;
    InvokeCallback(callback);
    returnPhase = 2;
}

extern "C" __declspec(dllexport) uint32_t WindowsNativeCleanupState()
{
    return (returnPhase << 8) | cleanupCount;
}
