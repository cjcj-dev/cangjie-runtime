// Observer only: registration must come from the real Object.init return bridge.
#include <atomic>
#include <cstdio>

std::atomic<unsigned> finalizerRegistrationCounts[3]{};
std::atomic<bool> finalizerRegistrationInvalidKind{false};

extern "C" void FinalizerRegistrationObserved(int kind)
{
    if (kind < 0 || kind >= 3) {
        finalizerRegistrationInvalidKind.store(true);
        return;
    }
    finalizerRegistrationCounts[kind].fetch_add(1);
}

extern "C" __attribute__((noinline)) void FinalizerRegistrationCheckpoint(int stage)
{
    std::fprintf(stderr, "FINALIZER_REGISTRATION_STAGE stage=%d\n", stage);
}

extern "C" long long FinalizerRegistrationCounts()
{
    const auto normal = finalizerRegistrationCounts[0].load();
    const auto before = finalizerRegistrationCounts[1].load();
    const auto after = finalizerRegistrationCounts[2].load();
    const bool invalid = finalizerRegistrationInvalidKind.load();
    std::fprintf(stderr,
        "FINALIZER_REGISTRATION_TARGET executed=1 normal=%u before_root_failure=%u after_root_failure=%u invalid=%d\n",
        normal, before, after, invalid);
    return normal == 1 && before == 0 && after == 1 && !invalid ? 0 : 1;
}
