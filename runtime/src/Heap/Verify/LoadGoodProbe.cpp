// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/LoadGoodProbe.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "Common/ColourMask.h"
#include "Heap/Verify/DiagGate.h"
#include "securec.h"

namespace MapleRuntime {
namespace LoadGoodProbe {
namespace {

bool EnvIsOne(const char* name)
{
    const char* value = std::getenv(name);
    return value != nullptr && value[0] == '1' && value[1] == '\0';
}

bool InitEnabled()
{
    if (EnvIsOne("MRT_GCV2_LOADGOOD")) {
        return true;
    }
    return DiagGate::TokenOn("loadgood");
}

// Every bucket is a separate question. Summing them across faces, or folding the
// cross-table cells back together, throws away the only thing the probe measures.
struct FaceCounters {
    std::atomic<uint64_t> reads;        // denominator: every call on this face
    std::atomic<uint64_t> nulls;        // slot held raw null; production returns early
    std::atomic<uint64_t> nonNull;      // reads - nulls; denominator of everything below

    // Predicate A -- ZGC ZPointer::is_load_bad (zAddress.inline.hpp:627-629).
    std::atomic<uint64_t> maskBad;
    // Predicate A', the decomposition of A into the two families g_cjLoadBadMask unions.
    std::atomic<uint64_t> taggedBits;   // mid-evacuation tag bits (TAGGED_BITS_MASK)
    std::atomic<uint64_t> staleRemap;   // a remap colour other than the one handed out now
    std::atomic<uint64_t> plain;        // no metadata bits at all: (word >> 48) == 0

    // Predicate B -- Collector::is_load_good (Collector.h:159-162), which additionally
    // demands *positive* remap colour in both generations. A plain word is neither
    // load-bad by A nor load-good by B; that gap is the whole story on the root face.
    std::atomic<uint64_t> notLoadGood;

    // Predicate C -- what production on this path actually uses.
    std::atomic<uint64_t> ghost;

    // A x C.
    std::atomic<uint64_t> badAndGhost;
    std::atomic<uint64_t> badNotGhost;
    std::atomic<uint64_t> ghostNotBad;
    std::atomic<uint64_t> neitherBadNorGhost;

    // B x C.
    std::atomic<uint64_t> nlgAndGhost;
    std::atomic<uint64_t> nlgNotGhost;
    std::atomic<uint64_t> ghostNotNlg;
    std::atomic<uint64_t> neitherNlgNorGhost;

    // Instrument health.
    std::atomic<uint64_t> maskZero;      // g_cjLoadBadMask was 0 at observation time
    std::atomic<uint64_t> synthBad;      // positive control, see NoteRead
    std::atomic<uint64_t> synthCtrlGood; // negative control of the same predicate
    std::atomic<uint64_t> hiBits;        // OR of every (word >> 48) seen on this face
};

FaceCounters g_face[kFaceCount];

// First few non-plain words per face, for forensics when a count is non-zero.
constexpr size_t kSampleCap = 8;
struct Samples {
    std::atomic<uint64_t> n;
    std::atomic<uint64_t> word[kSampleCap];
    std::atomic<uint64_t> mask[kSampleCap];
};
Samples g_sample[kFaceCount];

void WriteLine(const char* buf, size_t len)
{
    if (buf != nullptr && len > 0) {
        (void)write(STDERR_FILENO, buf, len);
    }
}

inline void Bump(std::atomic<uint64_t>& c) { c.fetch_add(1, std::memory_order_relaxed); }

const char* FaceName(uint8_t face) { return face == kFaceRoot ? "root" : "heap"; }

void ReportFace(const char* point, uint8_t face)
{
    const FaceCounters& f = g_face[face];
    char line[768];
    int n = sprintf_s(
        line, sizeof(line),
        "[GCV2][loadgood] point=%s face=%s mask=%#zx reads=%zu null=%zu nonnull=%zu "
        "mask_bad=%zu tagged=%zu stale_remap=%zu plain=%zu not_load_good=%zu ghost=%zu "
        "bad_and_ghost=%zu bad_not_ghost=%zu ghost_not_bad=%zu neither=%zu "
        "nlg_and_ghost=%zu nlg_not_ghost=%zu ghost_not_nlg=%zu nlg_neither=%zu "
        "mask_zero=%zu synth_bad=%zu synth_ctrl_good=%zu hi_bits_or=%#zx\n",
        point == nullptr ? "?" : point, FaceName(face),
        static_cast<size_t>(::g_cjLoadBadMask),
        static_cast<size_t>(f.reads.load(std::memory_order_relaxed)),
        static_cast<size_t>(f.nulls.load(std::memory_order_relaxed)),
        static_cast<size_t>(f.nonNull.load(std::memory_order_relaxed)),
        static_cast<size_t>(f.maskBad.load(std::memory_order_relaxed)),
        static_cast<size_t>(f.taggedBits.load(std::memory_order_relaxed)),
        static_cast<size_t>(f.staleRemap.load(std::memory_order_relaxed)),
        static_cast<size_t>(f.plain.load(std::memory_order_relaxed)),
        static_cast<size_t>(f.notLoadGood.load(std::memory_order_relaxed)),
        static_cast<size_t>(f.ghost.load(std::memory_order_relaxed)),
        static_cast<size_t>(f.badAndGhost.load(std::memory_order_relaxed)),
        static_cast<size_t>(f.badNotGhost.load(std::memory_order_relaxed)),
        static_cast<size_t>(f.ghostNotBad.load(std::memory_order_relaxed)),
        static_cast<size_t>(f.neitherBadNorGhost.load(std::memory_order_relaxed)),
        static_cast<size_t>(f.nlgAndGhost.load(std::memory_order_relaxed)),
        static_cast<size_t>(f.nlgNotGhost.load(std::memory_order_relaxed)),
        static_cast<size_t>(f.ghostNotNlg.load(std::memory_order_relaxed)),
        static_cast<size_t>(f.neitherNlgNorGhost.load(std::memory_order_relaxed)),
        static_cast<size_t>(f.maskZero.load(std::memory_order_relaxed)),
        static_cast<size_t>(f.synthBad.load(std::memory_order_relaxed)),
        static_cast<size_t>(f.synthCtrlGood.load(std::memory_order_relaxed)),
        static_cast<size_t>(f.hiBits.load(std::memory_order_relaxed)));
    if (n > 0) {
        WriteLine(line, static_cast<size_t>(n));
    }

    const uint64_t sn = g_sample[face].n.load(std::memory_order_relaxed);
    const uint64_t shown = sn < kSampleCap ? sn : kSampleCap;
    for (uint64_t i = 0; i < shown; ++i) {
        char row[192];
        int rn = sprintf_s(row, sizeof(row),
                           "[GCV2][loadgood] point=%s face=%s sample#%zu word=%#zx mask=%#zx\n",
                           point == nullptr ? "?" : point, FaceName(face), static_cast<size_t>(i),
                           static_cast<size_t>(g_sample[face].word[i].load(std::memory_order_relaxed)),
                           static_cast<size_t>(g_sample[face].mask[i].load(std::memory_order_relaxed)));
        if (rn > 0) {
            WriteLine(row, static_cast<size_t>(rn));
        }
    }
}

} // namespace

bool g_enabled = InitEnabled();

void NoteNull(uint8_t face)
{
    if (!g_enabled || face >= kFaceCount) {
        return;
    }
    Bump(g_face[face].reads);
    Bump(g_face[face].nulls);
}

void NoteRead(uint8_t face, uintptr_t word, bool ghost, bool loadGood)
{
    if (!g_enabled || face >= kFaceCount) {
        return;
    }
    FaceCounters& f = g_face[face];
    Bump(f.reads);
    Bump(f.nonNull);

    // Read the published mask once: it is republished at generation flips, so two reads
    // inside one observation could straddle a flip and produce a self-inconsistent row.
    const uintptr_t mask = ::g_cjLoadBadMask;
    const uintptr_t hi = word >> 48;
    f.hiBits.fetch_or(static_cast<uint64_t>(hi), std::memory_order_relaxed);

    const bool maskBad = (word & mask) != 0;
    if (maskBad) {
        Bump(f.maskBad);
    }
    if ((word & TAGGED_BITS_MASK) != 0) {
        Bump(f.taggedBits);
    }
    if ((word & (REMAP_COLOUR_MASK & mask)) != 0) {
        Bump(f.staleRemap);
    }
    if (hi == 0) {
        Bump(f.plain);
    } else {
        const uint64_t idx = g_sample[face].n.fetch_add(1, std::memory_order_relaxed);
        if (idx < kSampleCap) {
            g_sample[face].word[idx].store(static_cast<uint64_t>(word), std::memory_order_relaxed);
            g_sample[face].mask[idx].store(static_cast<uint64_t>(mask), std::memory_order_relaxed);
        }
    }
    if (!loadGood) {
        Bump(f.notLoadGood);
    }
    if (ghost) {
        Bump(f.ghost);
    }

    // A x C.
    if (maskBad && ghost) {
        Bump(f.badAndGhost);
    } else if (maskBad) {
        Bump(f.badNotGhost);
    } else if (ghost) {
        Bump(f.ghostNotBad);
    } else {
        Bump(f.neitherBadNorGhost);
    }

    // B x C.
    if (!loadGood && ghost) {
        Bump(f.nlgAndGhost);
    } else if (!loadGood) {
        Bump(f.nlgNotGhost);
    } else if (ghost) {
        Bump(f.ghostNotNlg);
    } else {
        Bump(f.neitherNlgNorGhost);
    }

    // Positive control, evaluated on real data with the live mask by the same predicate
    // that produced maskBad above. Setting one bit that the collector currently calls bad
    // must make the word test bad; clearing every bad bit must make it test good. If
    // synth_bad tracks nonnull and synth_ctrl_good tracks nonnull, then the predicate is
    // wired, the mask is non-zero, and the counters increment -- so a zero in mask_bad is
    // a statement about the data, not about the instrument.
    if (mask == 0) {
        Bump(f.maskZero);
    } else {
        const uintptr_t lowestBadBit = mask & (~mask + 1u);
        if (((word | lowestBadBit) & mask) != 0) {
            Bump(f.synthBad);
        }
        if (((word & ~mask) & mask) == 0) {
            Bump(f.synthCtrlGood);
        }
    }
}

void Report(const char* point)
{
    if (!g_enabled) {
        return;
    }
    for (uint8_t face = 0; face < kFaceCount; ++face) {
        ReportFace(point, face);
    }
}

} // namespace LoadGoodProbe
} // namespace MapleRuntime
