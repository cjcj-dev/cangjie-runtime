// Link this TU against the complete candidate runtime; never compile its sources here.
#include <cinttypes>
#include <cstdio>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include "Common/StackType.h"

#if !defined(__APPLE__) || !defined(__aarch64__) || !defined(ENABLE_BACKWARD_PTRAUTH_CFI)
#error This fixture requires ordinary Apple arm64 and the backward-PAC product layout
#endif

extern uintptr_t unwindPCForN2CStub;
extern "C" void pac1481_outside();
struct Sample { uintptr_t raw; uintptr_t signedPc; uintptr_t modifier; };
extern "C" void pac1481_produce(uintptr_t raw, Sample* out);

static bool Belongs(uintptr_t pc)
{
    MapleRuntime::MachineFrame frame(nullptr, reinterpret_cast<const uint32_t*>(pc));
    return frame.IsN2CStubFrame();
}

int main()
{
    for (uint32_t i = 0; i < _dyld_image_count(); ++i)
        std::printf("MAPPING %s\n", _dyld_get_image_name(i));
    // Integer addresses come from two independent linker relocations, before signing.
    const uintptr_t inputs[] = {reinterpret_cast<uintptr_t>(&unwindPCForN2CStub),
                                reinterpret_cast<uintptr_t>(&pac1481_outside)};
    Dl_info runtime{}, fixture{};
    const bool setup = dladdr(reinterpret_cast<void*>(inputs[0]), &runtime) &&
        dladdr(reinterpret_cast<void*>(inputs[1]), &fixture);
    // The approved recipe must also capture the linked consumer symbol/image identity.
    if (!setup || runtime.dli_fbase == fixture.dli_fbase) {
        std::puts("SETUP_FAILED: independent runtime/fixture images required");
        return 20;
    }
    std::printf("SETUP runtime=%s fixture=%s key=IA modifier=0\n",
                runtime.dli_fname, fixture.dli_fname);
    bool failed = false;
    bool distinguishable = false;
    for (unsigned i = 0; i < 2; ++i) {
        Sample sample{};
        std::printf("RAW_TRUTH target=%u address=%" PRIxPTR " key=IA modifier=0\n", i, inputs[i]);
        std::fflush(stdout);
        pac1481_produce(inputs[i], &sample); // Separate assembly TU; no sign/auth fold.
        const bool rawResult = Belongs(sample.raw);
        const bool signedResult = Belongs(sample.signedPc);
        const bool expected = i == 0;
        const bool pass = sample.raw == inputs[i] && sample.modifier == 0 &&
                          rawResult == expected && signedResult == expected;
        std::printf("ASSERT_EXECUTED target=%u raw=%" PRIxPTR " signed=%" PRIxPTR
                    " modifier=%" PRIxPTR " raw_result=%d signed_result=%d expected=%d pass=%d\n",
                    i, sample.raw, sample.signedPc, sample.modifier,
                    rawResult, signedResult, expected, pass);
        failed |= !pass;
        if (i == 0) distinguishable = sample.raw != sample.signedPc;
    }
    if (failed) return 1;
    if (!distinguishable) {
        std::puts("CUT_SENSITIVITY_NOT_RUN: fixed owned input signed==raw; stop, do not resample");
        return 21;
    }
    return 0;
}
