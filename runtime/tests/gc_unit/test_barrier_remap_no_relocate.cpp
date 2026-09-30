#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "Heap/z/zBarrier.inline.hpp"
#include <fstream>
#include <string>

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {
zpointer RemapInput(BaseObject* object, ZGeneration* generation)
{
    const uintptr_t remap = generation->id() == ZGenerationId::young
        ? (ZPointerRemappedOldMask & ~ZPointerRemappedYoungMask)
        : (ZPointerRemappedYoungMask & ~ZPointerRemappedOldMask);
    return ColouredPointer(object, ZPointer::remap_bits(remap));
}

size_t EntryCount(ZForwarding* forwarding)
{
    size_t count = 0;
    forwarding->for_each_from([&](MAddress) { ++count; });
    return count;
}

#if defined(__linux__) && !defined(NDEBUG)
template<typename Operation>
void ExpectContractAssertion(Operation operation, const char* expected)
{
    int descriptors[2];
    GC_EXPECT_EQ(pipe(descriptors), 0);
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        close(descriptors[0]);
        if (dup2(descriptors[1], STDERR_FILENO) < 0) { _exit(4); }
        close(descriptors[1]);
        std::ifstream mappings("/proc/self/maps");
        std::string mapping;
        while (std::getline(mappings, mapping)) {
            std::fprintf(stderr, "REMAP_CHILD_MAP %s\n", mapping.c_str());
        }
        operation();
        std::fprintf(stderr, "REMAP_CONTRACT_UNEXPECTED_RETURN\n");
        _exit(3);
    }
    close(descriptors[1]);
    std::string output;
    std::thread reader([&] {
        char buffer[4096];
        ssize_t length;
        while ((length = read(descriptors[0], buffer, sizeof(buffer))) != 0) {
            if (length > 0) { output.append(buffer, static_cast<size_t>(length)); }
            else if (errno != EINTR) { break; }
        }
        close(descriptors[0]);
    });
    JoinGuard readerGuard(reader);
    int status = 0;
    ChildVmTiming timing;
    const bool completed = WaitChildExit(child, status,
        std::chrono::steady_clock::now() + std::chrono::seconds(5), timing);
    reader.join();
    const bool matched = output.find(expected) != std::string::npos;
    std::fprintf(stderr, "%s", output.c_str());
    std::fprintf(stderr, "REMAP_CONTRACT_TARGET_ASSERT_EXECUTED expected=%s status=%d completed=%d matched=%d\n",
                 expected, status, completed, matched);
    GC_EXPECT_TRUE(completed && WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT && matched);
}
#endif
}

GC_TEST(BarrierRemap1327, PublishedEntryReturnsWithoutNewCopy)
{
    GcHeapFixture heap;
    heap.InstallPageOwner(heap.region0());
    ZGeneration* generation = heap.region0()->generation();
    ZForwarding* forwarding = generation->forwarding(heap.region0()->GetRegionStart());
    GC_EXPECT_TRUE(forwarding != nullptr);
    const zpointer input = RemapInput(heap.obj0, generation);
    GC_EXPECT_TRUE(ZBarrier::remap_generation(input) == generation);
    const size_t before = EntryCount(forwarding);
    const MAddress target = reinterpret_cast<MAddress>(heap.obj1);
    ForwardingCursor cursor = 0;
    forwarding->find(reinterpret_cast<MAddress>(heap.obj0), &cursor);
    GC_EXPECT_EQ(forwarding->insert(reinterpret_cast<MAddress>(heap.obj0), target, &cursor), target);
    const size_t published = EntryCount(forwarding);
    GC_EXPECT_EQ(published, before + 1);
    const zaddress result = ZBarrier::make_load_good_no_relocate(input);
    const size_t after = EntryCount(forwarding);
    std::fprintf(stderr, "REMAP_PUBLISHED_TARGET_ASSERT_EXECUTED result=%#zx expected=%#zx entries=%zu/%zu/%zu\n",
                 raw(result), target, before, published, after);
    GC_EXPECT_TRUE(raw(result) == target && after == published);
}

GC_TEST(BarrierRemap1327, PageOutsideRelocationSetStaysInPlace)
{
    GcHeapFixture heap;
    ZGeneration* generation = heap.region0()->generation();
    GC_EXPECT_TRUE(generation->forwarding(heap.region0()->GetRegionStart()) == nullptr);
    const zpointer input = RemapInput(heap.obj0, generation);
    GC_EXPECT_TRUE(ZBarrier::remap_generation(input) == generation);
    const zaddress result = ZBarrier::make_load_good_no_relocate(input);
    std::fprintf(stderr, "REMAP_IN_PLACE_TARGET_ASSERT_EXECUTED result=%#zx expected=%#zx\n",
                 raw(result), reinterpret_cast<MAddress>(heap.obj0));
    GC_EXPECT_TRUE(to_object(result) == heap.obj0);
}

GC_TEST(BarrierRemap1327, MissingEntryStopsAtForwardContract)
{
#if defined(__linux__) && defined(MRT_DEBUG) && MRT_DEBUG == 1 && !defined(NDEBUG)
    GcHeapFixture heap;
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(heap.region0(), heap.obj0));
    heap.InstallPageOwner(heap.region0());
    ZGeneration* generation = heap.region0()->generation();
    ZForwarding* forwarding = generation->forwarding(heap.region0()->GetRegionStart());
    GC_EXPECT_TRUE(forwarding != nullptr);
    GC_EXPECT_EQ(forwarding->find(reinterpret_cast<MAddress>(heap.obj0)), 0U);
    const zpointer input = RemapInput(heap.obj0, generation);
    GC_EXPECT_TRUE(ZBarrier::remap_generation(input) == generation);
    ExpectContractAssertion([&] {
        generation->set_phase(ZGenerationPhase::Relocate);
        const zaddress result = ZBarrier::make_load_good_no_relocate(input);
        std::fprintf(stderr, "REMAP_MISSING_ENTRY_RETURNED result=%#zx forwarding=%#zx entries=%zu\n",
                     raw(result), forwarding->find(reinterpret_cast<MAddress>(heap.obj0)), EntryCount(forwarding));
    }, "Check failed: to != 0");
#else
    std::fprintf(stderr, "REMAP_MISSING_ENTRY_NOT_RUN reason=product_assertions_disabled_or_non_linux\n");
#endif
}

GC_TEST(BarrierRemap1327, RelocatePathPublishesNewEntry)
{
    GcHeapFixture heap;
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(heap.region0(), heap.obj0));
    heap.InstallPageOwner(heap.region0());
    ZGeneration* generation = heap.region0()->generation();
    ZForwarding* forwarding = generation->forwarding(heap.region0()->GetRegionStart());
    GC_EXPECT_TRUE(forwarding != nullptr);
    GC_EXPECT_EQ(forwarding->find(reinterpret_cast<MAddress>(heap.obj0)), 0U);
    const zpointer input = RemapInput(heap.obj0, generation);
    GC_EXPECT_TRUE(ZBarrier::remap_generation(input) == generation);
    const size_t before = EntryCount(forwarding);
    const ZGenerationPhase previous = generation->phase();
    generation->set_phase(ZGenerationPhase::Relocate);
    const zaddress result = ZBarrier::make_load_good(input);
    generation->set_phase(previous);
    const size_t after = EntryCount(forwarding);
    std::fprintf(stderr, "REMAP_RELOCATE_POSITIVE_ASSERT_EXECUTED result=%#zx source=%#zx entries=%zu/%zu\n",
                 raw(result), reinterpret_cast<MAddress>(heap.obj0), before, after);
    GC_EXPECT_TRUE(to_object(result) != heap.obj0 && after == before + 1);
    GC_EXPECT_EQ(forwarding->find(reinterpret_cast<MAddress>(heap.obj0)), raw(result));
}

GC_TEST(BarrierRemap1327, PromotedFieldRequiresOldLoadGood)
{
#if defined(__linux__) && !defined(NDEBUG)
    const uintptr_t remap = ZPointerRemappedMask & ~ZPointerRemappedOldMask & ~ZPointerRemappedYoungMask;
    volatile zpointer slot = ZAddress::color(zaddress::null, ZPointer::remap_bits(remap));
    GC_EXPECT_FALSE(is_null(slot));
    ExpectContractAssertion([&] { ZBarrier::remap_young_relocated(&slot, slot); }, "ZPointer::is_old_load_good(o)");
#else
    std::fprintf(stderr, "REMAP_OLD_GUARD_NOT_RUN reason=product_assertions_disabled_or_non_linux\n");
#endif
}

GC_TEST(BarrierRemap1327, PromotedFieldRejectsYoungLoadGood)
{
#if defined(__linux__) && !defined(NDEBUG)
    GcHeapFixture heap;
    volatile zpointer slot = StoreGoodPointer(heap.obj0);
    GC_EXPECT_TRUE(ZPointer::is_old_load_good(slot) && ZPointer::is_young_load_good(slot));
    ExpectContractAssertion([&] { ZBarrier::remap_young_relocated(&slot, slot); }, "!ZPointer::is_young_load_good(o)");
#else
    std::fprintf(stderr, "REMAP_YOUNG_GUARD_NOT_RUN reason=product_assertions_disabled_or_non_linux\n");
#endif
}
