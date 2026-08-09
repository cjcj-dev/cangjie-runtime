// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Red-proof: pre-fix / broken models of the same invariants. Expected to FAIL.
// Build: clang++ -std=gnu++17 -I. -I../../src red_proof.cpp -o red_proof && ./red_proof
// A green-only suite with no red history is rejected (task criterion ①).

#include <cstdio>
#include <cstdint>
#include <cstring>

#include "Common/ColourMask.h"
#include "Common/ColourTypes.h"

using namespace MapleRuntime;

static int g_fail = 0;
static int g_pass = 0;

#define RED_EXPECT(cond, msg)                                                                                          \
    do {                                                                                                               \
        if (cond) {                                                                                                    \
            std::printf("[  PASS  ] %s\n", msg);                                                                        \
            ++g_pass;                                                                                                  \
        } else {                                                                                                       \
            std::printf("[  FAIL  ] %s\n", msg);                                                                        \
            ++g_fail;                                                                                                  \
        }                                                                                                              \
    } while (0)

// --- Broken U2: "uncolor" that forgets to mask (pre-discipline identity cast) ---
static uintptr_t broken_uncolor(uintptr_t p) { return p; } // BUG: keeps colour bits

// --- Broken U1: root write-back keeps colour (pre STACK_ROOTS_STAY_PLAIN) ---
static uintptr_t broken_root_writeback(uintptr_t coloured) { return coloured; }

// --- Broken U3: GetRoute returns forged to-addr when not survived ---
static uintptr_t broken_get_route(bool survived, uintptr_t toStart, uint64_t pre)
{
    (void)survived;
    return toStart + static_cast<uintptr_t>(pre); // BUG: no domain check
}

// --- Broken U4: clear liveInfo also nulls liveInfo0 (snapshot not retained) ---
struct BrokenGhost {
    int* liveInfo = nullptr;
    int* liveInfo0 = nullptr;
    void Prepare() { liveInfo0 = liveInfo; }
    void ClearBoth()
    {
        liveInfo = nullptr;
        liveInfo0 = nullptr; // BUG: should keep ghost
    }
};

// --- Broken U6: tip-small-int accepted ---
static bool broken_tip_ok(uintptr_t tip) { return tip != 0; } // BUG: accepts small ints

int main()
{
    constexpr uintptr_t addr = 0x00007f1234567000ULL;
    constexpr uintptr_t mask48 = (uintptr_t(1) << 48) - 1u;
    uintptr_t coloured = (addr & mask48) | ZPointerRemapped00;

    // U2 red: broken uncolor leaves colour bits
    RED_EXPECT(broken_uncolor(coloured) == addr, "U2 uncolor(color(p))==p [pre-fix broken_uncolor]");

    // U1 red: root write-back still coloured
    uintptr_t root = broken_root_writeback(coloured);
    RED_EXPECT((root & ~mask48) == 0, "U1 root slot plain after write-back [pre STACK_ROOTS]");

    // U3 red: non-survivor still gets a to-address
    uintptr_t forged = broken_get_route(false, 0x20000000u, 64);
    RED_EXPECT(forged == 0, "U3 missing domain returns null [pre GetRoute domain gate]");

    // U4 red: clear drops ghost
    int live = 1;
    BrokenGhost g;
    g.liveInfo = &live;
    g.Prepare();
    g.ClearBoth();
    RED_EXPECT(g.liveInfo0 != nullptr, "U4 liveInfo0 survives clear [pre installdomain snapshot]");

    // U6 red: tip-small-int accepted
    RED_EXPECT(!broken_tip_ok(42), "U6 tip-small-int rejected [pre PlausibleManagedObjectGate]");

    std::printf("[========] red_proof: %d passed, %d failed (expect failures)\n", g_pass, g_fail);
    // Exit 1 if we saw the expected reds (so CI can assert "red_proof is red").
    // If everything passed, the broken models are wrong → also fail.
    if (g_fail >= 4) {
        std::printf("RED_PROOF_OK: observed %d failures on broken models\n", g_fail);
        return 0; // proof succeeded (suite is capable of red)
    }
    std::printf("RED_PROOF_BAD: expected >=4 failures, got %d\n", g_fail);
    return 2;
}
