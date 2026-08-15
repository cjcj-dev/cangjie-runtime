#include "Heap/Verify/StackRootSlotAttest.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "Base/Log.h"
#include "Mutator/Mutator.h"

namespace MapleRuntime {
namespace StackRootSlotAttest {
namespace {

enum class Source : uint8_t {
    NONE,
    MINOR,
    MAJOR,
};

bool EnvIsOne(const char* name)
{
    const char* value = std::getenv(name);
    return value != nullptr && std::strcmp(value, "1") == 0;
}

bool InjectEnabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_VERIFY_STACK_ROOT_SLOTS_INJECT");
    return on;
}

Source ParseSource(const char* source)
{
    if (source != nullptr && std::strcmp(source, "minor") == 0) {
        return Source::MINOR;
    }
    if (source != nullptr && std::strcmp(source, "major") == 0) {
        return Source::MAJOR;
    }
    return Source::NONE;
}

const char* SourceName(Source source)
{
    switch (source) {
        case Source::MINOR:
            return "minor";
        case Source::MAJOR:
            return "major";
        case Source::NONE:
            return "none";
        default:
            return "none";
    }
}

std::atomic<Source> g_source{ Source::NONE };
std::atomic<size_t> g_checked{ 0 };
std::atomic<size_t> g_mismatched{ 0 };
std::atomic<size_t> g_naturalMismatched{ 0 };
std::atomic<bool> g_injected{ false };

thread_local size_t g_frameIndex = std::numeric_limits<size_t>::max();
thread_local bool g_frameIndexValid = false;
thread_local size_t g_suppressDepth = 0;

} // namespace

bool Enabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_VERIFY_STACK_ROOT_SLOTS");
    return on;
}

void Begin(const char* source)
{
    if (!Enabled()) {
        return;
    }
    Source parsed = ParseSource(source);
    if (parsed == Source::NONE) {
        return;
    }
    g_checked.store(0, std::memory_order_relaxed);
    g_mismatched.store(0, std::memory_order_relaxed);
    g_naturalMismatched.store(0, std::memory_order_relaxed);
    g_injected.store(false, std::memory_order_relaxed);
    g_source.store(parsed, std::memory_order_release);
}

void Finish()
{
    if (!Enabled()) {
        return;
    }
    Source source = g_source.exchange(Source::NONE, std::memory_order_acq_rel);
    if (source == Source::NONE) {
        return;
    }
    LOG(RTLOG_ERROR,
        "[GCV2][verify][stack-root-slots] SUMMARY source=%s checked=%zu mismatched=%zu natural=%zu injected=%d "
        "env=MRT_GCV2_VERIFY_STACK_ROOT_SLOTS=1",
        SourceName(source), g_checked.load(std::memory_order_relaxed),
        g_mismatched.load(std::memory_order_relaxed), g_naturalMismatched.load(std::memory_order_relaxed),
        g_injected.load(std::memory_order_relaxed) ? 1 : 0);
}

bool FrameActive()
{
    if (!Enabled()) {
        return false;
    }
    return g_suppressDepth == 0 && g_source.load(std::memory_order_acquire) != Source::NONE;
}

FrameScope::FrameScope(size_t frameIndex)
{
    if (!Enabled()) {
        return;
    }
    previousIndex_ = g_frameIndex;
    previousValid_ = g_frameIndexValid;
    g_frameIndex = frameIndex;
    g_frameIndexValid = true;
    engaged_ = true;
}

FrameScope::~FrameScope()
{
    if (!engaged_) {
        return;
    }
    g_frameIndex = previousIndex_;
    g_frameIndexValid = previousValid_;
}

SuppressScope::SuppressScope()
{
    if (!Enabled()) {
        return;
    }
    ++g_suppressDepth;
    engaged_ = true;
}

SuppressScope::~SuppressScope()
{
    if (engaged_) {
        --g_suppressDepth;
    }
}

void CheckFrame(uintptr_t framePC, bool mapValid, Mutator& mutator,
                const StackMapRootCounts& declared, const StackMapRootCounts& visited)
{
    if (!FrameActive()) {
        return;
    }
    Source source = g_source.load(std::memory_order_acquire);
    if (source == Source::NONE) {
        return;
    }

    g_checked.fetch_add(1, std::memory_order_relaxed);
    const bool naturalMismatch = declared.Base() != visited.Base() || declared.Derived() != visited.Derived();
    if (naturalMismatch) {
        g_naturalMismatched.fetch_add(1, std::memory_order_relaxed);
    }

    bool injectedHere = false;
    if (InjectEnabled()) {
        bool expected = false;
        injectedHere = g_injected.compare_exchange_strong(expected, true, std::memory_order_relaxed);
    }
    if (!naturalMismatch && !injectedHere) {
        return;
    }

    g_mismatched.fetch_add(1, std::memory_order_relaxed);
    const size_t frameIndex = g_frameIndexValid ? g_frameIndex : std::numeric_limits<size_t>::max();
    LOG(RTLOG_ERROR,
        "[GCV2][verify][stack-root-slots] MISMATCH source=%s frame=%zu framePC=%p "
        "declared=%zu visited=%zu declaredBase=%zu visitedBase=%zu declaredDerived=%zu visitedDerived=%zu "
        "declaredBaseSlots=%zu declaredBaseRegs=%zu visitedBaseSlots=%zu visitedBaseRegs=%zu "
        "declaredDerivedSlots=%zu declaredDerivedRegs=%zu mutator=%p tid=%u mapValid=%d natural=%d injected=%d "
        "env=MRT_GCV2_VERIFY_STACK_ROOT_SLOTS=1",
        SourceName(source), frameIndex, reinterpret_cast<void*>(framePC), declared.Total(), visited.Total(),
        declared.Base(), visited.Base(), declared.Derived(), visited.Derived(), declared.baseSlots,
        declared.baseRegs, visited.baseSlots, visited.baseRegs, declared.derivedSlots, declared.derivedRegs,
        &mutator, mutator.GetTid(), mapValid ? 1 : 0, naturalMismatch ? 1 : 0, injectedHere ? 1 : 0);
}

} // namespace StackRootSlotAttest
} // namespace MapleRuntime
