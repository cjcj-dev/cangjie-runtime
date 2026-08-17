#include "Heap/Verify/RelocAudit.h"

#include <cstring>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "Base/Log.h"
#include "Common/BaseObject.h"
#include "Heap/Heap.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
namespace RelocAudit {

struct Pair {
    BaseObject* from;
    BaseObject* to;
    uint32_t size;
};

struct ThreadLog {
    std::vector<Pair> pairs;
};

static thread_local ThreadLog* tlLog = nullptr;
static std::mutex gRegMu;
static std::vector<ThreadLog*> gLogs;

static ThreadLog* GetLog()
{
    ThreadLog* log = tlLog;
    if (log != nullptr) {
        return log;
    }
    log = new ThreadLog();
    tlLog = log;
    std::lock_guard<std::mutex> hold(gRegMu);
    gLogs.push_back(log);
    return log;
}

void Note(BaseObject* from, BaseObject* to, size_t size)
{
    if (!kEnabled) {
        return;
    }
    ThreadLog* log = GetLog();
    Pair p;
    p.from = from;
    p.to = to;
    p.size = static_cast<uint32_t>(size);
    log->pairs.push_back(p);
}

void CompareAtEvacFinish(const char* site)
{
    if (!kEnabled) {
        return;
    }
    size_t nPairs = 0;
    size_t nMismatch = 0;
    size_t nStateOnly = 0;
    size_t nHeaderType = 0;
    size_t nPayload = 0;
    size_t nRefOff = 0;
    size_t nPrimOff = 0;
    size_t nDiffBytes = 0;
    size_t offHist[8] = {};
    constexpr size_t kSample = 8;
    size_t samples = 0;

    std::lock_guard<std::mutex> hold(gRegMu);
    for (ThreadLog* log : gLogs) {
        nPairs += log->pairs.size();
        for (const Pair& p : log->pairs) {
            if (p.from == nullptr || p.to == nullptr || p.size < sizeof(uint64_t)) {
                continue;
            }
            if (!Heap::IsHeapAddress(p.from) || !Heap::IsHeapAddress(p.to)) {
                continue;
            }
            const uint8_t* a = reinterpret_cast<const uint8_t*>(p.from);
            const uint8_t* b = reinterpret_cast<const uint8_t*>(p.to);
            uint64_t wa;
            uint64_t wb;
            std::memcpy(&wa, a, sizeof(wa));
            std::memcpy(&wb, b, sizeof(wb));
            constexpr uint64_t kStateMask = 0x3ull << 48;
            const bool typeSame = ((wa & ~kStateMask) == (wb & ~kStateMask));
            const bool payloadSame = (p.size <= sizeof(uint64_t)) ||
                (std::memcmp(a + sizeof(uint64_t), b + sizeof(uint64_t), p.size - sizeof(uint64_t)) == 0);
            if (typeSame && payloadSame) {
                ++nStateOnly;
                continue;
            }
            ++nMismatch;
            if (!typeSame) {
                ++nHeaderType;
            }
            if (!payloadSame) {
                ++nPayload;
            }
            std::unordered_set<uint32_t> refOffs;
            if (p.to->HasRefField()) {
                p.to->ForEachRefField([&](RefField<>& field) {
                    intptr_t off = BaseObject::FieldOffset(p.to, &field);
                    if (off >= 0 && static_cast<size_t>(off) < p.size) {
                        refOffs.insert(static_cast<uint32_t>(off));
                    }
                });
            }
            bool logged = false;
            for (uint32_t off = 0; off < p.size; ++off) {
                if (a[off] == b[off]) {
                    continue;
                }
                if (off < 8 && typeSame) {
                    continue;
                }
                ++nDiffBytes;
                const uint32_t wordOff = off & ~7u;
                const bool isRef = refOffs.find(wordOff) != refOffs.end();
                if (isRef) {
                    ++nRefOff;
                } else {
                    ++nPrimOff;
                }
                const size_t bucket = (off < 8) ? 0 : (off < 16) ? 1 : (off < 32) ? 2 : (off < 64) ? 3 :
                    (off < 128) ? 4 : (off < 256) ? 5 : (off < 512) ? 6 : 7;
                ++offHist[bucket];
                if (samples < kSample && !logged) {
                    VLOG(REPORT,
                         "[RELOCAUDIT] mismatch site=%s from=%p to=%p size=%u firstOff=%u "
                         "fromB=%02x toB=%02x refField=%u",
                         site, p.from, p.to, p.size, off, a[off], b[off], static_cast<unsigned>(isRef));
                    ++samples;
                    logged = true;
                }
            }
        }
        log->pairs.clear();
    }
    VLOG(REPORT,
         "[RELOCAUDIT] site=%s pairs=%zu mismatch=%zu stateOnly=%zu headerType=%zu payload=%zu "
         "diffBytes=%zu refOff=%zu primOff=%zu "
         "off[0-7,8-15,16-31,32-63,64-127,128-255,256-511,512+]=%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu",
         site, nPairs, nMismatch, nStateOnly, nHeaderType, nPayload, nDiffBytes, nRefOff, nPrimOff,
         offHist[0], offHist[1], offHist[2], offHist[3], offHist[4], offHist[5], offHist[6], offHist[7]);
}

} // namespace RelocAudit
} // namespace MapleRuntime
