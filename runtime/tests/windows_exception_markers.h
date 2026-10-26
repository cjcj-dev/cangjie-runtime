// Addresses of production CALL-return labels exported from the runtime DLL.
// These labels reside in .text. A normal code import library can satisfy a
// plain extern with an EXE thunk; dllimport explicitly loads the DLL address
// from the IAT instead. Never dereference these address-only declarations.
#ifndef CANGJIE_TEST_WINDOWS_EXCEPTION_MARKERS_H
#define CANGJIE_TEST_WINDOWS_EXCEPTION_MARKERS_H

#include <cstdint>

extern "C" {
__declspec(dllimport) extern uintptr_t windowsC2NLeaveReturn;
__declspec(dllimport) extern uintptr_t windowsC2NOverflowReturn;
__declspec(dllimport) extern uintptr_t windowsC2NBodyStart;
__declspec(dllimport) extern uintptr_t windowsC2NBodyEnd;
__declspec(dllimport) extern uintptr_t windowsC2NFailureEnd;
__declspec(dllimport) extern uintptr_t windowsStackGrowBodyEnd;
__declspec(dllimport) extern uintptr_t windowsSafepointBodyEnd;
__declspec(dllimport) extern uintptr_t windowsReturnSafepointBodyEnd;
__declspec(dllimport) extern uintptr_t windowsExclusiveBodyEnd;
__declspec(dllimport) extern uintptr_t windowsStackOverflowBodyEnd;
}

#endif
