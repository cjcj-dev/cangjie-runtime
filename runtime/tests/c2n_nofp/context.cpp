// Read the real current ordinary-C2N context; never modify thread/GC state.
#include "Mutator/Mutator.h"
#include "Mutator/ThreadLocal.h"
#include <cstdint>
#include <dlfcn.h>
#include "RuntimeConfig.h"
extern "C" void* CJ_CJThreadStackGuardGet();
// The canonical label is intentionally local in the product export map.
// The runner derives this offset from the exact installed ELF symbol table.
#ifndef NOFP_CANONICAL_DELTA
#error Define NOFP_CANONICAL_DELTA from the installed runtime ELF
#endif
extern "C" int NoFPContext() {
    auto* tls = reinterpret_cast<MapleRuntime::ThreadLocalData*>(MapleRuntime::MRT_GetThreadLocalData());
    if (!tls || !tls->mutator) return 0;
    auto& frame = tls->mutator->GetUnwindContext().frameInfo.mFrame;
    const auto fa = reinterpret_cast<uintptr_t>(frame.GetFA());
    const auto guard = reinterpret_cast<uintptr_t>(CJ_CJThreadStackGuardGet());
    const auto entry = reinterpret_cast<uintptr_t>(dlsym(RTLD_DEFAULT, "CJ_MCC_C2NStub"));
    if (!entry || reinterpret_cast<uintptr_t>(frame.GetIP()) != entry + NOFP_CANONICAL_DELTA) return 1;
    if (!tls->mutator->InSaferegion()) return 2;
    if (!fa || fa < 240 || (guard && fa - 240 <= guard)) return 3;
    return 7;
}
extern "C" int FPContext(double control) {
    return control == 1.25 ? NoFPContext() : -1;
}
