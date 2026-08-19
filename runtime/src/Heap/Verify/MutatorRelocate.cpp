#include "Heap/Verify/MutatorRelocate.h"

#include <atomic>
#include <cstdio>

#include "Base/Log.h"

namespace MapleRuntime {
namespace MutatorRelocate {
// Product counters for 47595a33 self-relocate. Gate is compile-time (campaign
// cut MRT_GCV2_*). Enabled() stays kMutatorSelfRelocate — these numbers prove
// the leg still runs, they do not turn it off.
constexpr bool kStats = true;

static std::atomic<uint64_t> g_attempt{ 0 };
static std::atomic<uint64_t> g_retainOk{ 0 };
static std::atomic<uint64_t> g_fallback[static_cast<size_t>(Fallback::FALLBACK_COUNT)]{};
static std::atomic<uint64_t> g_alreadyFwd{ 0 };
static std::atomic<uint64_t> g_selfCopy[static_cast<size_t>(Role::ROLE_COUNT)]{};
static std::atomic<uint64_t> g_anyCopy[static_cast<size_t>(Role::ROLE_COUNT)]{};
static std::atomic<uint64_t> g_funnel[static_cast<size_t>(Role::ROLE_COUNT)]{};
static std::atomic<uint64_t> g_waitEnter{ 0 };
static std::atomic<uint64_t> g_waitGiveUp{ 0 };
static std::atomic<uint64_t> g_waitReceipt{ 0 };
static std::atomic<uint64_t> g_waitFatal{ 0 };
static thread_local bool t_inScope = false;
static std::atomic<bool> g_atexit{ false };

static void InstallAtexit()
{
    bool expected = false;
    if (g_atexit.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() { DumpSummary(); });
    }
}

bool Enabled() { return kMutatorSelfRelocate; }
bool DrainEnabled() { return false; }
bool StatsOn() { return kStats; }
bool InjectOn() { return false; }

void NoteAttempt()
{
    if (!kStats) {
        return;
    }
    InstallAtexit();
    const uint64_t n = g_attempt.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 8 || (n & (n - 1)) == 0) {
        LOG(RTLOG_ERROR, "[GCV2][mutrelo] attempt n=%llu retain=%llu",
            static_cast<unsigned long long>(n),
            static_cast<unsigned long long>(g_retainOk.load(std::memory_order_relaxed)));
    }
}

void NoteRetainOk()
{
    if (!kStats) {
        return;
    }
    g_retainOk.fetch_add(1, std::memory_order_relaxed);
}

void NoteFallback(Fallback why)
{
    if (!kStats) {
        return;
    }
    const size_t i = static_cast<size_t>(why);
    if (i < static_cast<size_t>(Fallback::FALLBACK_COUNT)) {
        g_fallback[i].fetch_add(1, std::memory_order_relaxed);
    }
}

void NoteAlreadyForwarded()
{
    if (!kStats) {
        return;
    }
    g_alreadyFwd.fetch_add(1, std::memory_order_relaxed);
}

bool InScope() { return t_inScope; }
void EnterScope() { t_inScope = true; }
void LeaveScope() { t_inScope = false; }

void NoteSelfCopy(size_t bytes, Role role)
{
    if (!kStats) {
        return;
    }
    (void)bytes;
    const size_t i = static_cast<size_t>(role);
    if (i < static_cast<size_t>(Role::ROLE_COUNT)) {
        g_selfCopy[i].fetch_add(1, std::memory_order_relaxed);
    }
}

void NoteAnyCopy(Role role)
{
    if (!kStats) {
        return;
    }
    const size_t i = static_cast<size_t>(role);
    if (i < static_cast<size_t>(Role::ROLE_COUNT)) {
        g_anyCopy[i].fetch_add(1, std::memory_order_relaxed);
    }
}

void NoteFunnelCall(Role role)
{
    if (!kStats) {
        return;
    }
    const size_t i = static_cast<size_t>(role);
    if (i < static_cast<size_t>(Role::ROLE_COUNT)) {
        g_funnel[i].fetch_add(1, std::memory_order_relaxed);
    }
}

void NoteWaitEnter()
{
    if (!kStats) {
        return;
    }
    g_waitEnter.fetch_add(1, std::memory_order_relaxed);
}

void NoteWaitGiveUp()
{
    if (!kStats) {
        return;
    }
    g_waitGiveUp.fetch_add(1, std::memory_order_relaxed);
}

void NoteWaitReceipt()
{
    if (!kStats) {
        return;
    }
    g_waitReceipt.fetch_add(1, std::memory_order_relaxed);
}

void NoteWaitFatal()
{
    if (!kStats) {
        return;
    }
    g_waitFatal.fetch_add(1, std::memory_order_relaxed);
}

void NoteDrain(Retire site, uint64_t spunNanos, bool contended)
{
    (void)site;
    (void)spunNanos;
    (void)contended;
}

void DumpSummary()
{
    if (!kStats) {
        return;
    }
    std::fprintf(stderr,
                 "[GCV2][mutrelo] atexit attempt=%llu retain=%llu alreadyFwd=%llu "
                 "fb_retain=%llu fb_copy=%llu fb_phase=%llu "
                 "self_mut=%llu self_gc=%llu self_rt=%llu "
                 "any_mut=%llu any_gc=%llu any_rt=%llu "
                 "funnel_mut=%llu funnel_gc=%llu "
                 "waitEnter=%llu waitGiveUp=%llu waitReceipt=%llu waitFatal=%llu\n",
                 static_cast<unsigned long long>(g_attempt.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_retainOk.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_alreadyFwd.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_fallback[0].load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_fallback[1].load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_fallback[2].load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_selfCopy[0].load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_selfCopy[1].load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_selfCopy[2].load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_anyCopy[0].load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_anyCopy[1].load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_anyCopy[2].load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_funnel[0].load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_funnel[1].load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_waitEnter.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_waitGiveUp.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_waitReceipt.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_waitFatal.load(std::memory_order_relaxed)));
    std::fflush(stderr);
}

} // namespace MutatorRelocate
} // namespace MapleRuntime
