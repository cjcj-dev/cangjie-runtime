// Observation-only probe for 1-bit tagID epoch ABA hypothesis (lane gctagid).

#include "TagEpochProbe.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Heap/Heap.h"

namespace MapleRuntime {

// C-linkage-ish entry used from RefField.inline.h / RefField.h without including this header.
void GctagidOnRefFieldWrite(const void* fieldAddr, MAddress newVal, uint32_t kind)
{
    TagEpochProbe::OnFieldWrite(fieldAddr, newVal, static_cast<TagEpochProbe::WriterKind>(kind));
}

std::atomic<uint64_t> TagEpochProbe::majorEpoch{0};
std::atomic<uint64_t> TagEpochProbe::minorEpoch{0};
std::atomic<uint64_t> TagEpochProbe::minorSinceLastMajor{0};
std::atomic<uint64_t> TagEpochProbe::majorTotal{0};
std::atomic<uint64_t> TagEpochProbe::minorTotal{0};
std::atomic<uint64_t> TagEpochProbe::tagWriteCount{0};
std::atomic<uint64_t> TagEpochProbe::probeFireCount{0};
std::atomic<uint64_t> TagEpochProbe::controlFireCount{0};
std::atomic<uint64_t> TagEpochProbe::staleCrashCount{0};
std::atomic<uint64_t> TagEpochProbe::deltaHist[kDeltaBuckets];
std::atomic<uint64_t> TagEpochProbe::writerPhaseHist[16];
std::atomic<uint64_t> TagEpochProbe::minorPerMajorHist[16];
std::atomic<uint64_t> TagEpochProbe::lastCrashMinorSinceMajor{0};
std::atomic<uint64_t> TagEpochProbe::lastCrashDelta{0};
std::atomic<uint32_t> TagEpochProbe::lastCrashPhase{0};
std::atomic<uint32_t> TagEpochProbe::lastCrashTagID{0};
std::atomic<uint64_t> TagEpochProbe::lastCrashTagMajor{0};
std::atomic<uintptr_t> TagEpochProbe::lastCrashField{0};
std::atomic<uintptr_t> TagEpochProbe::lastCrashTarget{0};
TagEpochProbe::Entry TagEpochProbe::table[kCapacity];
std::atomic<bool> TagEpochProbe::dumpedOnce{false};
std::atomic<bool> TagEpochProbe::enabled{true};

static inline size_t HashAddr(uintptr_t a)
{
    uint64_t x = static_cast<uint64_t>(a);
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return static_cast<size_t>(x & (TagEpochProbe::kCapacity - 1));
}

void TagEpochProbe::InitOnce()
{
    static std::atomic<bool> inited{false};
    bool expected = false;
    if (!inited.compare_exchange_strong(expected, true)) {
        return;
    }
    const char* env = std::getenv("MRT_GCTAGID_PROBE");
    if (env != nullptr && std::strcmp(env, "0") == 0) {
        enabled.store(false, std::memory_order_relaxed);
    }
    std::fprintf(stderr,
                 "[GCTAGID] PROBE_INIT capacity=%zu LOG_BOUNDED_openhash_overwrite_%zu_slots "
                 "sizeof_entry=%zu table_bytes=%zu enabled=%d\n",
                 kCapacity, kCapacity, sizeof(Entry), sizeof(table),
                 enabled.load(std::memory_order_relaxed) ? 1 : 0);
    std::fflush(stderr);
}

void TagEpochProbe::OnMajorFlip()
{
    if (!enabled.load(std::memory_order_relaxed)) {
        return;
    }
    InitOnce();
    uint64_t m = minorSinceLastMajor.exchange(0, std::memory_order_relaxed);
    size_t b = 0;
    if (m == 0) {
        b = 0;
    } else if (m == 1) {
        b = 1;
    } else if (m == 2) {
        b = 2;
    } else if (m == 3) {
        b = 3;
    } else {
        uint64_t v = m;
        b = 4;
        while (v > 7 && b < 15) {
            v >>= 1;
            ++b;
        }
    }
    minorPerMajorHist[b].fetch_add(1, std::memory_order_relaxed);
    majorEpoch.fetch_add(1, std::memory_order_relaxed);
    majorTotal.fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr, "[GCTAGID] MAJOR_FLIP majEpoch=%llu minorSincePrev=%llu totalMajor=%llu\n",
                 static_cast<unsigned long long>(majorEpoch.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(m),
                 static_cast<unsigned long long>(majorTotal.load(std::memory_order_relaxed)));
    std::fflush(stderr);
}

void TagEpochProbe::OnMinorEnd()
{
    if (!enabled.load(std::memory_order_relaxed)) {
        return;
    }
    InitOnce();
    minorEpoch.fetch_add(1, std::memory_order_relaxed);
    minorTotal.fetch_add(1, std::memory_order_relaxed);
    minorSinceLastMajor.fetch_add(1, std::memory_order_relaxed);
}

void TagEpochProbe::OnFieldWrite(const void* fieldAddr, MAddress newVal, WriterKind kind)
{
    if (!enabled.load(std::memory_order_relaxed) || fieldAddr == nullptr) {
        return;
    }
    if (!ValIsTagged(newVal)) {
        return;
    }
    InitOnce();
    uintptr_t key = reinterpret_cast<uintptr_t>(fieldAddr);
    size_t idx = HashAddr(key);
    Entry& e = table[idx];
    e.fieldAddr.store(key, std::memory_order_relaxed);
    uint64_t maj = majorEpoch.load(std::memory_order_relaxed);
    uint64_t min = minorEpoch.load(std::memory_order_relaxed);
    e.majorEpoch.store(maj, std::memory_order_relaxed);
    e.minorEpoch.store(min, std::memory_order_relaxed);
    e.tagID.store(ValTagID(newVal), std::memory_order_relaxed);
    uint32_t phase = 0;
    // GetGCPhase is safe once heap exists; phase 0 if early.
    phase = static_cast<uint32_t>(Heap::GetHeap().GetGCPhase());
    e.phase.store(phase, std::memory_order_relaxed);
    e.writerKind.store(static_cast<uint32_t>(kind), std::memory_order_relaxed);
    e.seq.store(tagWriteCount.fetch_add(1, std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    if (phase < 16) {
        writerPhaseHist[phase].fetch_add(1, std::memory_order_relaxed);
    }
}

bool TagEpochProbe::Lookup(uintptr_t fieldAddr, uint64_t& maj, uint64_t& min, uint32_t& tag, uint32_t& phase,
                           uint32_t& kind, uint64_t& seq)
{
    size_t idx = HashAddr(fieldAddr);
    Entry& e = table[idx];
    if (e.fieldAddr.load(std::memory_order_relaxed) != fieldAddr) {
        return false;
    }
    maj = e.majorEpoch.load(std::memory_order_relaxed);
    min = e.minorEpoch.load(std::memory_order_relaxed);
    tag = e.tagID.load(std::memory_order_relaxed);
    phase = e.phase.load(std::memory_order_relaxed);
    kind = e.writerKind.load(std::memory_order_relaxed);
    seq = e.seq.load(std::memory_order_relaxed);
    return true;
}

void TagEpochProbe::OnTaggedSeen(const void* fieldAddr, MAddress fieldVal, const char* site)
{
    if (!enabled.load(std::memory_order_relaxed)) {
        return;
    }
    controlFireCount.fetch_add(1, std::memory_order_relaxed);
    (void)fieldAddr;
    (void)fieldVal;
    (void)site;
}

void TagEpochProbe::OnPreFindToVersion(const void* fieldAddr, BaseObject* target, MAddress fieldVal, const char* site)
{
    if (!enabled.load(std::memory_order_relaxed)) {
        return;
    }
    InitOnce();
    probeFireCount.fetch_add(1, std::memory_order_relaxed);
    OnTaggedSeen(fieldAddr, fieldVal, site);

    bool inHeap = Heap::IsHeapAddress(target);
    uintptr_t key = reinterpret_cast<uintptr_t>(fieldAddr);
    uint64_t majNow = majorEpoch.load(std::memory_order_relaxed);
    uint64_t minNow = minorEpoch.load(std::memory_order_relaxed);
    uint64_t msm = minorSinceLastMajor.load(std::memory_order_relaxed);

    uint64_t tagMaj = 0;
    uint64_t tagMin = 0;
    uint32_t tagId = ValTagID(fieldVal);
    uint32_t phase = 0;
    uint32_t kind = 0;
    uint64_t seq = 0;
    bool found = fieldAddr != nullptr && Lookup(key, tagMaj, tagMin, tagId, phase, kind, seq);
    // if lookup miss, still use live field bits for tagId
    if (!found) {
        tagId = ValTagID(fieldVal);
    }
    uint64_t delta = found ? (majNow >= tagMaj ? majNow - tagMaj : 0) : UINT64_MAX;

    if (!inHeap) {
        staleCrashCount.fetch_add(1, std::memory_order_relaxed);
        lastCrashMinorSinceMajor.store(msm, std::memory_order_relaxed);
        lastCrashDelta.store(delta, std::memory_order_relaxed);
        lastCrashPhase.store(phase, std::memory_order_relaxed);
        lastCrashTagID.store(ValTagID(fieldVal), std::memory_order_relaxed);
        lastCrashTagMajor.store(tagMaj, std::memory_order_relaxed);
        if (fieldAddr != nullptr) {
            lastCrashField.store(key, std::memory_order_relaxed);
        }
        lastCrashTarget.store(reinterpret_cast<uintptr_t>(target), std::memory_order_relaxed);
        if (delta != UINT64_MAX) {
            size_t b = delta < (kDeltaBuckets - 1) ? static_cast<size_t>(delta) : (kDeltaBuckets - 1);
            deltaHist[b].fetch_add(1, std::memory_order_relaxed);
        } else {
            deltaHist[kDeltaBuckets - 1].fetch_add(1, std::memory_order_relaxed);
        }
        char deltaStr[32];
        if (delta == UINT64_MAX) {
            std::snprintf(deltaStr, sizeof(deltaStr), "MISS");
        } else {
            std::snprintf(deltaStr, sizeof(deltaStr), "%llu", static_cast<unsigned long long>(delta));
        }
        std::fprintf(stderr,
                     "[GCTAGID] STALE_TAG_HIT site=%s field=%p target=%p fieldVal=%#zx tagged=%d tagID=%u "
                     "found=%d tagMajor=%llu tagMinor=%llu majNow=%llu minNow=%llu delta=%s msm=%llu "
                     "writePhase=%u writeKind=%u seq=%llu\n",
                     site != nullptr ? site : "?", fieldAddr, target, static_cast<size_t>(fieldVal),
                     ValIsTagged(fieldVal) ? 1 : 0, ValTagID(fieldVal), found ? 1 : 0,
                     static_cast<unsigned long long>(tagMaj), static_cast<unsigned long long>(tagMin),
                     static_cast<unsigned long long>(majNow), static_cast<unsigned long long>(minNow), deltaStr,
                     static_cast<unsigned long long>(msm), phase, kind, static_cast<unsigned long long>(seq));
        std::fflush(stderr);
        DumpSummary("pre_find_to_version_nonheap");
    }
}

void TagEpochProbe::DumpSummary(const char* reason)
{
    char deltaBuf[256];
    size_t off = 0;
    for (size_t i = 0; i < kDeltaBuckets && off + 24 < sizeof(deltaBuf); ++i) {
        uint64_t v = deltaHist[i].load(std::memory_order_relaxed);
        int n = std::snprintf(deltaBuf + off, sizeof(deltaBuf) - off, "%s%zu:%llu", i ? "," : "", i,
                              static_cast<unsigned long long>(v));
        if (n > 0) {
            off += static_cast<size_t>(n);
        }
    }

    char phaseBuf[256];
    off = 0;
    for (size_t i = 0; i < 16 && off + 24 < sizeof(phaseBuf); ++i) {
        uint64_t v = writerPhaseHist[i].load(std::memory_order_relaxed);
        if (v == 0) {
            continue;
        }
        int n = std::snprintf(phaseBuf + off, sizeof(phaseBuf) - off, "%s%zu:%llu", off ? "," : "", i,
                              static_cast<unsigned long long>(v));
        if (n > 0) {
            off += static_cast<size_t>(n);
        }
    }
    if (off == 0) {
        std::snprintf(phaseBuf, sizeof(phaseBuf), "empty");
    }

    char mpmBuf[256];
    off = 0;
    for (size_t i = 0; i < 16 && off + 24 < sizeof(mpmBuf); ++i) {
        uint64_t v = minorPerMajorHist[i].load(std::memory_order_relaxed);
        if (v == 0) {
            continue;
        }
        int n = std::snprintf(mpmBuf + off, sizeof(mpmBuf) - off, "%s%zu:%llu", off ? "," : "", i,
                              static_cast<unsigned long long>(v));
        if (n > 0) {
            off += static_cast<size_t>(n);
        }
    }
    if (off == 0) {
        std::snprintf(mpmBuf, sizeof(mpmBuf), "empty");
    }

    std::fprintf(stderr,
                 "[GCTAGID] SUMMARY reason=%s majorTotal=%llu minorTotal=%llu majEpoch=%llu minEpoch=%llu "
                 "tagWrites=%llu probeFires=%llu controlFires=%llu staleCrash=%llu "
                 "STALE_TAG_EPOCH_DELTA_{%s} MINOR_PER_MAJOR_{%s} lastCrashMsm=%llu lastCrashDelta=%llu "
                 "lastCrashWritePhase=%u lastCrashTagID=%u lastCrashTagMajor=%llu lastField=%p lastTarget=%p "
                 "TAG_WRITER_PHASE_{%s}\n",
                 reason != nullptr ? reason : "?",
                 static_cast<unsigned long long>(majorTotal.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(minorTotal.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(majorEpoch.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(minorEpoch.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(tagWriteCount.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(probeFireCount.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(controlFireCount.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(staleCrashCount.load(std::memory_order_relaxed)), deltaBuf, mpmBuf,
                 static_cast<unsigned long long>(lastCrashMinorSinceMajor.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(lastCrashDelta.load(std::memory_order_relaxed)),
                 lastCrashPhase.load(std::memory_order_relaxed), lastCrashTagID.load(std::memory_order_relaxed),
                 static_cast<unsigned long long>(lastCrashTagMajor.load(std::memory_order_relaxed)),
                 reinterpret_cast<void*>(lastCrashField.load(std::memory_order_relaxed)),
                 reinterpret_cast<void*>(lastCrashTarget.load(std::memory_order_relaxed)), phaseBuf);
    std::fflush(stderr);
    (void)dumpedOnce;
}

} // namespace MapleRuntime
