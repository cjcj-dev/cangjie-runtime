#ifndef MRT_REGION_LIFE_DIAG_H
#define MRT_REGION_LIFE_DIAG_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
class RegionInfo;

// regionlife: account region free/take with a lifeId (anti address-reuse).
// Gate: MRT_GCV2_REGIONLIFE=1 or MRT_GCV2_WHOZERO=1 (whozero joins free on CAS-null).
// Default off. Observation only — does not change reclaim.
namespace RegionLifeDiag {

// path codes (free edge attribution)
enum FreePath : uint16_t {
    PATH_COLLECT_GENERIC = 0,
    PATH_FWD_KNOWN_EMPTY = 1,   // ForwardRegion → IsKnownEmpty → CollectRegion
    PATH_FWD_AFTER_COPY = 2,    // ForwardRegion success tail → CollectRegion
    PATH_PINNED_GARBAGE = 3,    // CollectPinnedGarbage → CollectRegion
    PATH_LARGE_GARBAGE = 4,     // CollectLargeGarbage → CollectRegion
    PATH_RELEASE_LARGE = 5,     // ReleaseRegion (large threshold)
    PATH_UNUSED_PINNED = 6,     // AllocPinned unused region handback
    PATH_OTHER = 7,
    // Sampled before CollectLargeGarbage evaluates !IsSurvivedObject(0), so it is the
    // only path whose mark bit is not already constrained to 0 by our own gate. Kept
    // distinct from PATH_LARGE_GARBAGE precisely so the two never get pooled.
    PATH_PRE_RELEASE_DECISION = 8,
};

bool Enabled();
// True when free ring is recorded (REGIONLIFE or WHOZERO).
bool FreeTrackOn();

void NoteTake(RegionInfo* region);
void NoteFree(RegionInfo* region, uint16_t path);
void NoteRelease(RegionInfo* region, uint16_t path);

// Set by callers around CollectRegion / ReleaseRegion; cleared after NoteFree.
void SetNextFreePath(uint16_t path);
uint16_t TakeNextFreePath(); // returns and resets to GENERIC

// Lookup last free covering addr; returns true if a row was found.
// sameLife: free.lifeId still matches take-map for free.start (no reuse since free).
bool LookupLastFree(uintptr_t addr, uint32_t* freeSeq, uint32_t* lifeId, uint16_t* path, uint16_t* phase,
                    uint32_t* gcCount, uintptr_t* start, uintptr_t* end, uint8_t* knownEmpty, uint64_t* liveBytes,
                    uint8_t* young, uint8_t* neverExam, uint8_t* auth, uint8_t* sameLife, uint32_t* takeLifeNow);

// Dump one-line join for whozeroExact / crash.
void DumpJoinForTarget(uintptr_t tgt, const char* tag);

void Report(const char* point);

} // namespace RegionLifeDiag
} // namespace MapleRuntime

#endif
