#include "Heap/Verify/PermWhoAdmit.h"

#include <atomic>

#include "Base/Log.h"

namespace MapleRuntime {
namespace PermWhoAdmit {
// This file was hollowed out to `return false` / empty bodies while the header kept its full
// four-arm contract, its gate names (MRT_GCV2_PERMWHO_ADMIT) and all of its call sites.  That
// combination reads exactly like a live instrument: turning the documented gate on produces zero
// output, and zero output reads as "the arm never fires".  RouteDestHold.cpp was hollowed the same
// way (e90e22a4) and had to be restored earlier in this campaign; ToverFailDiag.cpp still is.
//
// Only the abandon sink is restored here, because that is the one number in question.  The gate is
// a compile-time constant, not a new MRT_GCV2_ variable: the campaign cut those from 190 to 3.
//
// What is being counted, in the header's own words (PermWhoAdmit.h:71-77): the abandon arm at
// RegionManager.cpp:2758-2806 assumes "RouteObject miss => mutator keeps from (valid)", which holds
// only for objects that pass did not copy.  Objects it did copy already carry ObjectState::FORWARDED
// in their own header and nothing clears it; the region then survives as UNMOVABLE_FROM and
// RegionInfo.h:1155 wipes the RouteInfo those headers were receipts for.
//
// So forwardedObjects > 0 means the abandon arm leaves stale FORWARDED receipts with no route, which
// is both observed crash families at once:
//   the compiler loads the header as one 64-bit word, so stateCode FORWARDED = 3 at bits 48-49
//   becomes (3 << 48) inside an address -> non-canonical -> #GP, si_code=128, si_addr=(nil)
//   and once the region is reclaimed the payload is zeroed -> null base + field offset, si_code=1
//
// OpenJDK cannot reach this state: a page is chosen for relocation once, when the relocation set is
// selected, and relocation is never abandoned in a way that leaves mutators on the from-copy.  A
// forwarding stays reachable until its ref count drops (ZForwarding::detach_page,
// zForwarding.cpp:171-181), and every pre-relocation pointer is load-bad until remapped, so a stale
// from-address can never look good.
constexpr bool kPermWhoAdmit = true;

static std::atomic<uint64_t> g_abandons{ 0 };
static std::atomic<uint64_t> g_abandonsWithForwarded{ 0 };
static std::atomic<uint64_t> g_forwardedTotal{ 0 };
static std::atomic<uint64_t> g_walkedTotal{ 0 };

bool Enabled() { return kPermWhoAdmit; }

void NoteRoute(RegionInfo* region, BaseObject* from, BaseObject* to) {}
void NoteRoutePlan(RegionInfo* region, size_t fromBytes, unsigned densifyOutcome) {}

void NoteAbandon(RegionInfo* region, size_t walkedObjects, size_t forwardedObjects)
{
    if (!kPermWhoAdmit) {
        return;
    }
    const uint64_t n = g_abandons.fetch_add(1, std::memory_order_relaxed) + 1;
    g_walkedTotal.fetch_add(walkedObjects, std::memory_order_relaxed);
    if (forwardedObjects == 0) {
        // Positive control: proves the walk reached the sink even when nothing is stale, so a zero
        // in the line below cannot be confused with a dead probe.
        if (n == 1) {
            LOG(RTLOG_ERROR, "[PERMWHO][abandon] armed first n=1 walked=%zu forwarded=0", walkedObjects);
        }
        return;
    }
    const uint64_t bad = g_abandonsWithForwarded.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint64_t fwdTotal = g_forwardedTotal.fetch_add(forwardedObjects, std::memory_order_relaxed) +
        forwardedObjects;
    if ((bad & (bad - 1)) != 0) { // powers of two: the subject dies by SIGSEGV, so no atexit
        return;
    }
    LOG(RTLOG_ERROR,
        "[PERMWHO][abandon] stale_receipts region=%p walked=%zu forwarded=%zu | abandons=%lu "
        "with_forwarded=%lu forwarded_total=%lu walked_total=%lu",
        static_cast<void*>(region), walkedObjects, forwardedObjects, n, bad, fwdTotal,
        g_walkedTotal.load(std::memory_order_relaxed));
}

void DumpSummary()
{
    if (!kPermWhoAdmit) {
        return;
    }
    LOG(RTLOG_ERROR, "[PERMWHO][summary] abandons=%lu with_forwarded=%lu forwarded_total=%lu walked_total=%lu",
        g_abandons.load(std::memory_order_relaxed), g_abandonsWithForwarded.load(std::memory_order_relaxed),
        g_forwardedTotal.load(std::memory_order_relaxed), g_walkedTotal.load(std::memory_order_relaxed));
}
} // namespace PermWhoAdmit
} // namespace MapleRuntime
