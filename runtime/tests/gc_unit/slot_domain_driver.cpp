#include "Heap/z/zAddress.hpp"
#include "Heap/z/zHeap.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

using namespace MapleRuntime;

extern "C" void slot_domain_write(void *val, void *base, void **field);

static bool is_colored_store_good(uintptr_t word)
{
    return (word & ZPointerStoreBadMask) == 0 && (word & ZPointerStoreGoodMask) == ZPointerStoreGoodMask;
}

int main()
{
    ZGlobalsPointers::initialize();
    const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    const size_t span = page * 3;
    void *arena = mmap(nullptr, span, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (arena == MAP_FAILED) {
        std::fprintf(stderr, "SLOT_DOMAIN_FAIL mmap\n");
        return 2;
    }
    auto *base = reinterpret_cast<uintptr_t *>(arena);
    uintptr_t r0s = reinterpret_cast<uintptr_t>(base);
    uintptr_t r0e = r0s + page;
    uintptr_t hole = r0e;
    uintptr_t r1s = r0e + page;
    uintptr_t r1e = r1s + page;
    Heap::OnHeapCreated(r0s, { { r0s, r0e }, { r1s, r1e } });
    Heap::OnHeapExtended(r1e);

    std::printf("SLOT_DOMAIN_RANGES n=%lu r0=%lx-%lx hole=%lx r1=%lx-%lx start=%lx end=%lx\n",
                static_cast<unsigned long>(g_cjHeapRangeCount), r0s, r0e, hole, r1s, r1e,
                static_cast<unsigned long>(g_cjHeapStart), static_cast<unsigned long>(g_cjHeapEnd));

    if (g_cjHeapRangeCount < 2) {
        std::fprintf(stderr, "SLOT_DOMAIN_FAIL range_count\n");
        return 3;
    }

    void *payload = reinterpret_cast<void *>(r1s + 16);
    void **heap_place = reinterpret_cast<void **>(r0s + 32);
    void **hole_place = reinterpret_cast<void **>(hole + 32);
    void **glob_mem = reinterpret_cast<void **>(mmap(nullptr, page, PROT_READ | PROT_WRITE,
                                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (glob_mem == MAP_FAILED) {
        std::fprintf(stderr, "SLOT_DOMAIN_FAIL glob mmap\n");
        return 2;
    }
    *heap_place = nullptr;
    *hole_place = nullptr;
    *glob_mem = nullptr;

    void *heap_holder = reinterpret_cast<void *>(r0s + 64);
    void *null_holder = nullptr;
    void *glob_holder = reinterpret_cast<void *>(static_cast<uintptr_t>(1));

    slot_domain_write(payload, heap_holder, heap_place);
    uintptr_t heap_dyn = *reinterpret_cast<uintptr_t *>(heap_place);
    *heap_place = nullptr;
    slot_domain_write(payload, null_holder, heap_place);
    uintptr_t heap_null = *reinterpret_cast<uintptr_t *>(heap_place);
    slot_domain_write(payload, heap_holder, hole_place);
    uintptr_t hole_word = *reinterpret_cast<uintptr_t *>(hole_place);
    slot_domain_write(payload, glob_holder, glob_mem);
    uintptr_t glob_word = *reinterpret_cast<uintptr_t *>(glob_mem);

    std::printf("SLOT_DOMAIN_WORDS heap_dyn=%lx heap_null=%lx hole=%lx glob=%lx payload=%p storeGood=%lx storeBad=%lx\n",
                heap_dyn, heap_null, hole_word, glob_word, payload, ZPointerStoreGoodMask, ZPointerStoreBadMask);

    const uintptr_t raw = reinterpret_cast<uintptr_t>(payload);
    int rc = 0;
    if (!is_colored_store_good(heap_dyn) || (heap_dyn & ~ZPointerAllMetadataMask) == 0) {
        std::fprintf(stderr, "SLOT_DOMAIN_EXIST heap_dyn not colored\n");
        rc = 10;
    }
    std::printf("SLOT_DOMAIN_EXIST heap_dyn_colored=1\n");
    if (heap_null != raw && !is_colored_store_good(heap_null)) {
        std::fprintf(stderr, "SLOT_DOMAIN_EXIST heap_null neither plain nor colored\n");
        rc = 11;
    }
    std::printf("SLOT_DOMAIN_EXIST heap_null_word=%lx\n", heap_null);

    if (glob_word != raw) {
        std::fprintf(stderr, "SLOT_DOMAIN_FAIL glob expected plain raw=%lx got=%lx\n", raw, glob_word);
        rc = 12;
    } else {
        std::printf("SLOT_DOMAIN_OK glob_plain\n");
    }

    if (hole_word != raw) {
        std::fprintf(stderr, "SLOT_DOMAIN_FAIL hole expected plain raw=%lx got=%lx\n", raw, hole_word);
        rc = rc ? rc : 13;
    } else {
        std::printf("SLOT_DOMAIN_OK hole_plain\n");
    }
    return rc;
}
