// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zRelocationSetSelector.hpp"
#include "Heap/z/zRelocationSetSelector.inline.hpp"
#include "Heap/z/zPage.inline.hpp"
#include "Heap/z/zPageAllocator.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zStat.hpp"
#include "Mutator/ThreadLocal.h"
#include "gc_unittest.hpp"
#include "zunittest.hpp"

#include <csignal>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(RelocationSetSelector, EmptySet)
{
    ZRelocationSetSelector selector(ZYoungCompactionLimit);
    selector.select();
    GC_EXPECT_EQ(selector.selected_small()->length(), 0);
    GC_EXPECT_EQ(selector.selected_medium()->length(), 0);
    GC_EXPECT_EQ(selector.forwarding_entries(), static_cast<size_t>(0));
}

GC_TEST(RelocationSetSelector, SelectOrderLargeMediumSmall)
{
    ZRelocationSetSelector selector(ZFragmentationLimit);
    selector.select();
    GC_EXPECT_TRUE(selector.empty_pages()->length() == 0);
}

namespace {
struct SelectorPageFixture {
    HeapParam heapParam{};
    std::unique_ptr<ZTestRegionHeap> heapHolder;
    RegionManager manager;
    SelectorPageFixture()
    {
        ThreadLocal::SetThreadType(ThreadType::FP_THREAD);
        ZStat::Initialize();
        heapParam.regionSize = 2048;
        heapParam.exemptionThreshold = 0.8;
        heapHolder.reset(new ZTestRegionHeap(4096, manager, heapParam, 0.5));
    }
    ZPage* takeSmall()
    {
        const size_t n = ZPageSizeSmall / ZPage::UNIT_SIZE;
        return manager.TakeRegion(n, ZPageType::small, false, false, false);
    }
};
}

GC_TEST(RelocationSetSelector, PreFilterDropsLowGarbage)
{
    SelectorPageFixture fx;
    ZPage* page = fx.takeSmall();
    GC_EXPECT_TRUE(page != nullptr);
    page->inc_live(1, page->size() - 8);
    ZRelocationSetSelector selector(5.0);
    selector.register_live_page(page);
    selector.select();
    GC_EXPECT_EQ(selector.selected_small()->length(), 0);
}

GC_TEST(RelocationSetSelector, SemiSortUsesPartitionFingers)
{
    SelectorPageFixture fx;
    ZPage* highLive = fx.takeSmall();
    ZPage* lowLive = fx.takeSmall();
    GC_EXPECT_TRUE(highLive != nullptr && lowLive != nullptr);
    highLive->inc_live(1, highLive->size() / 4);
    lowLive->inc_live(1, 64);
    ZRelocationSetSelector selector(0.0);
    selector.register_live_page(lowLive);
    selector.register_live_page(highLive);
    selector.select();
    GC_EXPECT_TRUE(selector.selected_small()->length() >= 2);
    GC_EXPECT_TRUE(selector.selected_small()->at(0) == lowLive);
}

GC_TEST(RelocationSetSelector, FragmentationLimitStopsPrefix)
{
    SelectorPageFixture fx;
    ZPage* a = fx.takeSmall();
    ZPage* b = fx.takeSmall();
    GC_EXPECT_TRUE(a != nullptr && b != nullptr);
    a->inc_live(1, 64);
    b->inc_live(1, 64);
    ZRelocationSetSelector tight(99.0);
    tight.register_live_page(a);
    tight.register_live_page(b);
    tight.select();
    GC_EXPECT_EQ(tight.selected_small()->length(), 0);
    ZRelocationSetSelector loose(0.0);
    loose.register_live_page(a);
    loose.register_live_page(b);
    loose.select();
    GC_EXPECT_TRUE(loose.selected_small()->length() >= 1);
}

// TakeHeadRegion unlinks before ForwardClaimedPage (zPageAllocator.inline.hpp:317-330).
// IsFromRegion is then false; relocatable pages remain IsLoneFromRegion.
// Allocating pages match neither. Fail-closed is ZGeneration::select_relocation_set
// skip (zGeneration.cpp:1471) plus check_selected_relocatable after select(),
// not a silent skip at ForwardClaimedPage.
GC_TEST(RelocationSetSelector, AllocatingUnlinkedIsNotLoneFrom)
{
    SelectorPageFixture fx;
    ZPage* page = fx.takeSmall();
    GC_EXPECT_TRUE(page != nullptr);
    GC_EXPECT_TRUE(page->is_allocating());
    GC_EXPECT_FALSE(page->is_relocatable());
    GC_EXPECT_FALSE(page->IsFromRegion());
    GC_EXPECT_FALSE(page->IsLoneFromRegion());
}

GC_TEST(RelocationSetSelector, CheckSelectedRejectsAllocating)
{
    SelectorPageFixture fx;
    ZPage* page = fx.takeSmall();
    GC_EXPECT_TRUE(page != nullptr);
    GC_EXPECT_TRUE(page->is_allocating());
    int childStderr[2];
    GC_EXPECT_EQ(pipe(childStderr), 0);
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        close(childStderr[0]);
        if (dup2(childStderr[1], STDERR_FILENO) < 0) {
            _exit(126);
        }
        close(childStderr[1]);
        (void)signal(SIGABRT, SIG_DFL);
        ZRelocationSetSelector selector;
        selector.add_selected_small(page, 1);
        selector.check_selected_relocatable();
        _exit(0);
    }
    close(childStderr[1]);
    std::string transcript;
    char buffer[512];
    for (;;) {
        const ssize_t count = read(childStderr[0], buffer, sizeof(buffer));
        if (count <= 0) {
            break;
        }
        transcript.append(buffer, static_cast<size_t>(count));
    }
    close(childStderr[0]);
    (void)std::fwrite(transcript.data(), 1, transcript.size(), stderr);
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    GC_EXPECT_TRUE(WIFSIGNALED(status));
    GC_EXPECT_EQ(WTERMSIG(status), SIGABRT);
    GC_EXPECT_TRUE(transcript.find("selected page must be relocatable") != std::string::npos);
}


