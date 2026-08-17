#ifndef MRT_PIN_FIRE_DIAG_H
#define MRT_PIN_FIRE_DIAG_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
class RegionInfo;

// pinfire: count whether raw-pointer pin reclaim path ran / blocked.
// Gate: MRT_GCV2_PINFIRE=1 (exact). Default off. Report-only; no reclaim decision change.
namespace PinFireDiag {

bool Enabled();

void NoteAddRawPointer();
void NoteCollectPinnedGarbage();
// early-return from CollectFreePinnedSlots while count > 0
void NoteSkipFreeSlots(RegionInfo* region);
// whole-region skip in CollectPinnedGarbage while count > 0
void NoteSkipRegion(RegionInfo* region);

void Report(const char* point);

} // namespace PinFireDiag
} // namespace MapleRuntime

#endif
