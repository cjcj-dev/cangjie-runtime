#include "Heap/z/zResurrection.hpp"

#include "Base/Panic.h"
#include "Mutator/MutatorManager.h"

namespace MapleRuntime {

std::atomic<bool> ZResurrection::blocked{ false };

void ZResurrection::block()
{
#ifdef MRT_DEBUG
    CHECK_DETAIL(MutatorManager::Instance().WorldStopped(), "Should be at safepoint");
#endif
    blocked.store(true, std::memory_order_release);
}

void ZResurrection::unblock()
{
    blocked.store(false, std::memory_order_relaxed);
}

} // namespace MapleRuntime
