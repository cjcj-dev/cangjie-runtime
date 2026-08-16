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

    // A x B -- the two predicates that both claim to mean "load bad".
    //
    // ZGC has one: is_load_bad(p) = untype(p) & ZPointerLoadBadMask (zAddress.inline.hpp:627).
    // We have two, and they are not the same set. g_cjLoadBadMask includes TAGGED_BITS_MASK
    // (ColourMask.h ComputeBadMasks), but Collector::is_load_good never consults it: it is
    // !is_null && (raw & RemappedYoungMask) && (raw & RemappedOldMask) (Collector.h:159,
    // WCollector.h:212-221). So a mid-evacuation reference carrying the current remap colour
    // is load-bad by the mask and load-good by the predicate. maskbad_not_nlg counts exactly
    // that population -- the references the six barrier fast paths hand back untouched.
    std::atomic<uint64_t> maskbadAndNlg;
    std::atomic<uint64_t> maskbadNotNlg;   // ZGC mask says bad, our predicate says good
    std::atomic<uint64_t> nlgNotMaskbad;   // our predicate says bad, mask says good (plain)
    std::atomic<uint64_t> neitherAB;

    // The other two published masks, on the same word. The load mask covers only part of
    // the colour space; a word can be load-good and still mark-bad or store-bad.
    std::atomic<uint64_t> markBad;
    std::atomic<uint64_t> storeBad;
    std::atomic<uint64_t> loadGoodMarkBad;   // load says fine, mark does not
    std::atomic<uint64_t> loadGoodStoreBad;

    // Per-bit census of bits 48..63, over every non-null read and over the load-bad subset.
    // A boolean "bad colour" hides which family the bits belong to; this does not.
    std::atomic<uint64_t> bitAll[16];
    std::atomic<uint64_t> bitBad[16];

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

// Before/after rings. Capped and lossy on purpose: the question is "what do these look
// like", not "how many are there" -- the counts above already answer that.
constexpr size_t kPairCap = 24;
struct PairRing {
    std::atomic<uint64_t> n;
    std::atomic<uint64_t> word[kPairCap];
    std::atomic<uint64_t> stripped[kPairCap];
    std::atomic<uint64_t> resolved[kPairCap];
};
PairRing g_badRing[kFaceCount];
PairRing g_routeRing[kFaceCount];
// route ring summary over every event, not just the sampled ones.
std::atomic<uint64_t> g_routeMoved[kFaceCount];   // resolved != stripped
std::atomic<uint64_t> g_routeSame[kFaceCount];    // resolved == stripped
std::atomic<uint64_t> g_routeNull[kFaceCount];    // resolved == 0

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

    char ab[512];
    int abn = sprintf_s(ab, sizeof(ab),
                        "[GCV2][loadgood] point=%s face=%s load_mask=%#zx mark_mask=%#zx "
                        "store_mask=%#zx maskbad_and_nlg=%zu maskbad_not_nlg=%zu "
                        "nlg_not_maskbad=%zu neither_ab=%zu mark_bad=%zu store_bad=%zu "
                        "loadgood_markbad=%zu loadgood_storebad=%zu\n",
                        point == nullptr ? "?" : point, FaceName(face),
                        static_cast<size_t>(::g_cjLoadBadMask), static_cast<size_t>(::g_cjMarkBadMask),
                        static_cast<size_t>(::g_cjStoreBadMask),
                        static_cast<size_t>(f.maskbadAndNlg.load(std::memory_order_relaxed)),
                        static_cast<size_t>(f.maskbadNotNlg.load(std::memory_order_relaxed)),
                        static_cast<size_t>(f.nlgNotMaskbad.load(std::memory_order_relaxed)),
                        static_cast<size_t>(f.neitherAB.load(std::memory_order_relaxed)),
                        static_cast<size_t>(f.markBad.load(std::memory_order_relaxed)),
                        static_cast<size_t>(f.storeBad.load(std::memory_order_relaxed)),
                        static_cast<size_t>(f.loadGoodMarkBad.load(std::memory_order_relaxed)),
                        static_cast<size_t>(f.loadGoodStoreBad.load(std::memory_order_relaxed)));
    if (abn > 0) {
        WriteLine(ab, static_cast<size_t>(abn));
    }
    {
        char hist[640];
        int off = sprintf_s(hist, sizeof(hist), "[GCV2][loadgood] point=%s face=%s bitcensus",
                            point == nullptr ? "?" : point, FaceName(face));
        for (unsigned b = 0; b < 16u && off > 0; ++b) {
            const uint64_t all = f.bitAll[b].load(std::memory_order_relaxed);
            const uint64_t bad = f.bitBad[b].load(std::memory_order_relaxed);
            if (all == 0) {
                continue;
            }
            int n2 = sprintf_s(hist + off, sizeof(hist) - static_cast<size_t>(off),
                               " b%u=%zu/%zu", 48u + b, static_cast<size_t>(all),
                               static_cast<size_t>(bad));
            off = n2 > 0 ? off + n2 : -1;
        }
        if (off > 0) {
            int n3 = sprintf_s(hist + off, sizeof(hist) - static_cast<size_t>(off), "\n");
            if (n3 > 0) {
                WriteLine(hist, static_cast<size_t>(off + n3));
            }
        }
    }

    char sum[256];
    int sn2 = sprintf_s(sum, sizeof(sum),
                        "[GCV2][loadgood] point=%s face=%s route_moved=%zu route_same=%zu "
                        "route_null=%zu bad_pairs=%zu route_pairs=%zu\n",
                        point == nullptr ? "?" : point, FaceName(face),
                        static_cast<size_t>(g_routeMoved[face].load(std::memory_order_relaxed)),
                        static_cast<size_t>(g_routeSame[face].load(std::memory_order_relaxed)),
                        static_cast<size_t>(g_routeNull[face].load(std::memory_order_relaxed)),
                        static_cast<size_t>(g_badRing[face].n.load(std::memory_order_relaxed)),
                        static_cast<size_t>(g_routeRing[face].n.load(std::memory_order_relaxed)));
    if (sn2 > 0) {
        WriteLine(sum, static_cast<size_t>(sn2));
    }
    for (int which = 0; which < 2; ++which) {
        PairRing& ring = which == 0 ? g_badRing[face] : g_routeRing[face];
        const char* tag = which == 0 ? "bad" : "route";
        const uint64_t rn = ring.n.load(std::memory_order_relaxed);
        const uint64_t rshown = rn < kPairCap ? rn : kPairCap;
        for (uint64_t i = 0; i < rshown; ++i) {
            const uint64_t w = ring.word[i].load(std::memory_order_relaxed);
            const uint64_t s = ring.stripped[i].load(std::memory_order_relaxed);
            const uint64_t r = ring.resolved[i].load(std::memory_order_relaxed);
            const long long delta = static_cast<long long>(r) - static_cast<long long>(s);
            char row[256];
            int rn2 = sprintf_s(row, sizeof(row),
                                "[GCV2][loadgood] point=%s face=%s %s#%zu word=%#zx "
                                "stripped=%#zx resolved=%#zx delta=%lld\n",
                                point == nullptr ? "?" : point, FaceName(face), tag,
                                static_cast<size_t>(i), static_cast<size_t>(w),
                                static_cast<size_t>(s), static_cast<size_t>(r), delta);
            if (rn2 > 0) {
                WriteLine(row, static_cast<size_t>(rn2));
            }
        }
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

    // A x B. maskbad_not_nlg is the leak: ZGC's predicate rejects it, ours admits it.
    if (maskBad && !loadGood) {
        Bump(f.maskbadAndNlg);
    } else if (maskBad) {
        Bump(f.maskbadNotNlg);
    } else if (!loadGood) {
        Bump(f.nlgNotMaskbad);
    } else {
        Bump(f.neitherAB);
    }

    // The rest of the colour space, on the same word.
    const bool markBadNow = (word & ::g_cjMarkBadMask) != 0;
    const bool storeBadNow = (word & ::g_cjStoreBadMask) != 0;
    if (markBadNow) {
        Bump(f.markBad);
        if (!maskBad) {
            Bump(f.loadGoodMarkBad);
        }
    }
    if (storeBadNow) {
        Bump(f.storeBad);
        if (!maskBad) {
            Bump(f.loadGoodStoreBad);
        }
    }

    for (unsigned b = 0; b < 16u; ++b) {
        if (((hi >> b) & 1u) != 0) {
            Bump(f.bitAll[b]);
            if (maskBad) {
                Bump(f.bitBad[b]);
            }
        }
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

void NoteBadSample(uint8_t face, uintptr_t word, uintptr_t stripped)
{
    if (!g_enabled || face >= kFaceCount) {
        return;
    }
    const uint64_t idx = g_badRing[face].n.fetch_add(1, std::memory_order_relaxed);
    if (idx < kPairCap) {
        g_badRing[face].word[idx].store(static_cast<uint64_t>(word), std::memory_order_relaxed);
        g_badRing[face].stripped[idx].store(static_cast<uint64_t>(stripped), std::memory_order_relaxed);
        g_badRing[face].resolved[idx].store(static_cast<uint64_t>(stripped), std::memory_order_relaxed);
    }
}

void NoteRouteSample(uint8_t face, uintptr_t word, uintptr_t stripped, uintptr_t resolved)
{
    if (!g_enabled || face >= kFaceCount) {
        return;
    }
    if (resolved == 0) {
        Bump(g_routeNull[face]);
    } else if (resolved != stripped) {
        Bump(g_routeMoved[face]);
    } else {
        Bump(g_routeSame[face]);
    }
    const uint64_t idx = g_routeRing[face].n.fetch_add(1, std::memory_order_relaxed);
    if (idx < kPairCap) {
        g_routeRing[face].word[idx].store(static_cast<uint64_t>(word), std::memory_order_relaxed);
        g_routeRing[face].stripped[idx].store(static_cast<uint64_t>(stripped), std::memory_order_relaxed);
        g_routeRing[face].resolved[idx].store(static_cast<uint64_t>(resolved), std::memory_order_relaxed);
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
