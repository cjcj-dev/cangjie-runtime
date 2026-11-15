// Read the real current ordinary-C2N context; never modify thread/GC state.
#include "Mutator/Mutator.h"
#include "Mutator/ThreadLocal.h"
#include <cstdint>
#include "RuntimeConfig.h"
extern "C" void* CJ_CJThreadStackGuardGet();
extern "C" uintptr_t unwindPCForC2NStub;
extern "C" int NoFPContext() {
    auto* tls = reinterpret_cast<MapleRuntime::ThreadLocalData*>(MapleRuntime::MRT_GetThreadLocalData());
    if (!tls || !tls->mutator) return 0;
    auto& frame = tls->mutator->GetUnwindContext().frameInfo.mFrame;
    const auto fa = reinterpret_cast<uintptr_t>(frame.GetFA());
    const auto guard = reinterpret_cast<uintptr_t>(CJ_CJThreadStackGuardGet());
    if (reinterpret_cast<uintptr_t>(frame.GetIP()) != reinterpret_cast<uintptr_t>(&unwindPCForC2NStub)) return 1;
    if (!tls->mutator->InSaferegion()) return 2;
    if (!fa || fa < 240 || (guard && fa - 240 <= guard)) return 3;
    return 7;
}
extern "C" int FPContext(double control) {
    return control == 1.25 ? NoFPContext() : -1;
}
