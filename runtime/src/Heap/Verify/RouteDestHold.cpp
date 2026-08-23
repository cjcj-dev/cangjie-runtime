#include "Heap/Verify/RouteDestHold.h"

#include "Heap/Allocator/RegionInfo.h"
namespace MapleRuntime {
namespace RouteDestHold {
bool AccountOn() { return false; }
bool InjectHandbackOn() { return false; }
// ⛔⛔⛔⛔⛔ NOT a probe -- this is a load-bearing product guard, and e90e22a4
// ("hollow out default-off Verify probe bodies") deleted it by stubbing this file to
// `return false`.  The pre-hollowing body was default-ON:
//
//     if (region == nullptr || !region->IsRouteDestHeld()) return false;
//     if (AccountOn())        { ...counters... }        // env-gated, counting only
//     if (InjectHandbackOn()) { return false; }         // env-gated negative arm
//     return true;                                      // <- the default answer
//
// Only the counting and the negative arm were env-gated; the structural predicate always
// applied.  Its caller says what it is for (RegionManager.cpp:1132-1137): "a region a
// published route still names must not enter the collection set ... it is about address
// ownership: reclaiming it hands its units back for ClearUnits while the route keeps
// answering the old geometry."
//
// With HoldsBack() == false that is exactly what happened: cjcj::cjc --package
// packages/basic/src on a coloured runtime read a field whose target sat in a region that
// had been taken as garbage, ClearUnits-zeroed and re-initialised, and faulted on its
// zeroed TypeInfo (SIGSEGV si_addr=0x8, `cmpb $0x15,0x8(%rdi)` with rdi == 0).  The stale
// value carried the current remap colour, so the barrier fast path returned it untouched --
// unlike ZGC, where a stale pointer is load-bad and cannot be dereferenced without the
// barrier consulting the forwarding table first.
//
// Restored without the two env-gated arms: those environment variables no longer exist.
bool HoldsBack(const RegionInfo* region, Site) { return region != nullptr && region->IsRouteDestHeld(); }
void NoteReuse(const RegionInfo* region, bool held) {  }
void NoteReclaimFunnel(const RegionInfo* region, const char* site) {  }
void NoteClearPoint(size_t heldRegions, size_t heldBytes) {  }
void NoteTo2Resolve(uintptr_t arith, uint32_t idx) {  }
void DumpSummary() {  }
} // namespace RouteDestHold
} // namespace MapleRuntime
