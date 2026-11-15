// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
// Mutator::RecordStackDerived is the stack-grow collection of one stack-map
// derived pair. A null-$BP stack record inside the moved part of the old
// CJThread stack is recorded as its own base at offset 0 and FixExtendedStack's
// rebase arithmetic moves it by stackOffset; every other stack derived pointer
// keeps the non-null-base CHECK, and non-stack addresses are not recorded.
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <string>
#include <tuple>
#include <vector>
#include "Heap/z/zAddress.inline.hpp"
#include "Mutator/Mutator.h"
#include "ObjectModel/RefField.h"
#include "gc_unittest.hpp"
#if defined(__linux__)
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(__linux__)
using namespace MapleRuntime;
namespace {
using DerivedSlots = std::vector<std::tuple<DerivedSlot*, BasePtrType, size_t>>;
constexpr intptr_t STACK_OFFSET = 0x100000;
constexpr size_t WORDS = 64;
constexpr size_t MOVED_FROM = 16; // words[0, 16) model the uncopied area below the grow frame
uintptr_t g_globalWord = 0;       // an address outside every stack

struct OldStack {
    alignas(16) uintptr_t words[WORDS] {};
    uintptr_t derivedWord = 0;
    Mutator owner;
    OldStack()
    {
        owner.SetStackTopAddr(Addr(0));
        owner.SetStackSize(sizeof(words));
        owner.SetStackBaseAddr(Addr(WORDS));
    }
    uintptr_t Addr(size_t index) const
    {
        return reinterpret_cast<uintptr_t>(&words[0]) + index * sizeof(uintptr_t);
    }
    DerivedSlots Record(uintptr_t base, uintptr_t derived)
    {
        derivedWord = derived;
        DerivedSlots slots;
        owner.RecordStackDerived(slots, to_zaddress_unsafe(base),
                                 DerivedSlotAt(static_cast<void*>(&derivedWord)), Addr(MOVED_FROM));
        return slots;
    }
    // FixExtendedStack's derived loop with the copied word modelled in place.
    static uintptr_t Rebase(const std::tuple<DerivedSlot*, BasePtrType, size_t>& entry)
    {
        auto& [slot, oldBase, offset] = entry;
        RootSlot newBase;
        StorePlain(newBase, to_zaddress(raw(oldBase) + STACK_OFFSET));
        RebaseDerived(*slot, newBase, offset);
        return raw(slot->LoadDerived());
    }
};

void ExpectRejected(uintptr_t (*base)(OldStack&), uintptr_t (*derived)(OldStack&), const char* name)
{
    int output[2];
    GC_EXPECT_EQ(pipe(output), 0);
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        close(output[0]);
        if (dup2(output[1], STDERR_FILENO) < 0) { _exit(126); }
        close(output[1]);
        signal(SIGABRT, SIG_DFL);
        OldStack stack;
        DerivedSlots slots = stack.Record(base(stack), derived(stack));
        std::fprintf(stderr, "NULL_BASE_ACCEPTED %s entries=%zu\n", name, slots.size());
        _exit(0);
    }
    close(output[1]);
    std::string transcript;
    char buffer[512];
    ssize_t count;
    while ((count = read(output[0], buffer, sizeof(buffer))) > 0) { transcript.append(buffer, count); }
    close(output[0]);
    std::fwrite(transcript.data(), 1, transcript.size(), stderr);
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    const bool rejected = WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT &&
        transcript.find("stack derived pointer must have a non-null base at or below it") != std::string::npos;
    std::fprintf(stderr, "NULL_BASE_REJECT %s status=%d rejected=%d\n", name, status, rejected);
    GC_EXPECT_TRUE(rejected);
}
}

GC_OTHER_VM_TEST(StackGrowNullBase, MovedRecordFollowsCopy)
{
    OldStack stack;
    const uintptr_t record = stack.Addr(40);
    DerivedSlots slots = stack.Record(0, record);
    GC_EXPECT_EQ(slots.size(), size_t(1));
    GC_EXPECT_EQ(raw(std::get<1>(slots[0])), record);
    GC_EXPECT_EQ(std::get<2>(slots[0]), size_t(0));
    GC_EXPECT_EQ(OldStack::Rebase(slots[0]), record + STACK_OFFSET);
}

GC_OTHER_VM_TEST(StackGrowNullBase, NonNullStackBaseUnchanged)
{
    OldStack stack;
    DerivedSlots slots = stack.Record(stack.Addr(30), stack.Addr(34));
    GC_EXPECT_EQ(slots.size(), size_t(1));
    GC_EXPECT_EQ(raw(std::get<1>(slots[0])), stack.Addr(30));
    GC_EXPECT_EQ(std::get<2>(slots[0]), size_t(4 * sizeof(uintptr_t)));
    GC_EXPECT_EQ(OldStack::Rebase(slots[0]), stack.Addr(34) + STACK_OFFSET);
}

GC_OTHER_VM_TEST(StackGrowNullBase, OutsideStackNotRecorded)
{
    OldStack stack;
    const uintptr_t global = reinterpret_cast<uintptr_t>(&g_globalWord);
    GC_EXPECT_EQ(stack.Record(1, global).size(), size_t(0));             // x86 global marker base
    GC_EXPECT_EQ(stack.Record(0, global).size(), size_t(0));             // null base, native/global word
    GC_EXPECT_EQ(stack.Record(0, stack.Addr(WORDS)).size(), size_t(0));  // first word past the old stack
    GC_EXPECT_EQ(stack.Record(0, 0).size(), size_t(0));
}

GC_OTHER_VM_TEST(StackGrowNullBase, NullBaseBelowMovedPartRejected)
{
    ExpectRejected([](OldStack&) { return uintptr_t(0); }, [](OldStack& s) { return s.Addr(8); }, "below-moved");
}

GC_OTHER_VM_TEST(StackGrowNullBase, NonNullBaseAboveDerivedRejected)
{
    ExpectRejected([](OldStack& s) { return s.Addr(40); }, [](OldStack& s) { return s.Addr(34); }, "base-above");
}
#endif
