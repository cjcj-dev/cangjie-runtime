#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zGlobals.hpp"
#include "Heap/z/zInitialize.hpp"
#include "Heap/z/zPhysicalMemoryBacking_bsd.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/mman.h>

namespace MapleRuntime {
extern size_t ZBackingOffsetMax;
}

static size_t page_size()
{
    const long page = sysconf(_SC_PAGESIZE);
    return page > 0 ? static_cast<size_t>(page) : static_cast<size_t>(4096);
}

static void prepare()
{
    unsetenv("MRT_LOG_PATH");
    unsetenv("cjUseLargePages");
    MapleRuntime::ZBackingOffsetMax = static_cast<size_t>(-1);
}

static int ctor_ok()
{
    prepare();
    const size_t n = page_size();
    MapleRuntime::ZPhysicalMemoryBacking backing(n);
    if (!backing.is_initialized() || MapleRuntime::ZInitialize::had_error()) {
        std::fprintf(stderr, "CASE ctor_ok fail initialized=%d had_error=%d\n",
                     backing.is_initialized() ? 1 : 0, MapleRuntime::ZInitialize::had_error() ? 1 : 0);
        return 1;
    }
    std::fprintf(stderr, "CASE ctor_ok ok\n");
    return 0;
}

static int ctor_fail()
{
    prepare();
    MapleRuntime::ZPhysicalMemoryBacking backing(0);
    if (backing.is_initialized() || !MapleRuntime::ZInitialize::had_error()) {
        std::fprintf(stderr, "CASE ctor_fail fail initialized=%d had_error=%d\n",
                     backing.is_initialized() ? 1 : 0, MapleRuntime::ZInitialize::had_error() ? 1 : 0);
        return 1;
    }
    std::fprintf(stderr, "CASE ctor_fail ok\n");
    return 0;
}

static int map_ok()
{
    prepare();
    const size_t n = page_size();
    MapleRuntime::ZPhysicalMemoryBacking backing(n);
    if (!backing.is_initialized()) {
        std::fprintf(stderr, "CASE map_ok setup ctor\n");
        return 2;
    }
    if (backing.commit(MapleRuntime::to_zbacking_offset(static_cast<uintptr_t>(0)), n, 0) != n) {
        std::fprintf(stderr, "CASE map_ok setup commit\n");
        return 2;
    }
    void* dest = mmap(nullptr, n, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (dest == MAP_FAILED) {
        std::fprintf(stderr, "CASE map_ok setup dest\n");
        return 2;
    }
    backing.map(MapleRuntime::to_zaddress_unsafe(reinterpret_cast<uintptr_t>(dest)), n,
                MapleRuntime::to_zbacking_offset(static_cast<uintptr_t>(0)));
    std::fprintf(stderr, "CASE map_ok returned\n");
    return 0;
}

static int map_fail()
{
    prepare();
    const size_t n = page_size();
    MapleRuntime::ZPhysicalMemoryBacking backing(n);
    if (!backing.is_initialized()) {
        std::fprintf(stderr, "CASE map_fail setup ctor\n");
        return 2;
    }
    void* dest = mmap(nullptr, n, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (dest == MAP_FAILED) {
        std::fprintf(stderr, "CASE map_fail setup dest\n");
        return 2;
    }
    backing.map(MapleRuntime::to_zaddress_unsafe(reinterpret_cast<uintptr_t>(dest)), n,
                MapleRuntime::to_zbacking_offset(static_cast<uintptr_t>(1) << 30));
    std::fprintf(stderr, "CASE map_fail returned\n");
    return 0;
}

static int unmap_ok()
{
    prepare();
    const size_t n = page_size();
    MapleRuntime::ZPhysicalMemoryBacking backing(n);
    if (!backing.is_initialized()) {
        std::fprintf(stderr, "CASE unmap_ok setup ctor\n");
        return 2;
    }
    void* page = mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (page == MAP_FAILED) {
        std::fprintf(stderr, "CASE unmap_ok setup page\n");
        return 2;
    }
    backing.unmap(MapleRuntime::to_zaddress_unsafe(reinterpret_cast<uintptr_t>(page)), n);
    std::fprintf(stderr, "CASE unmap_ok returned\n");
    return 0;
}

static int unmap_fail()
{
    prepare();
    const size_t n = page_size();
    MapleRuntime::ZPhysicalMemoryBacking backing(n);
    if (!backing.is_initialized()) {
        std::fprintf(stderr, "CASE unmap_fail setup ctor\n");
        return 2;
    }
    void* page = mmap(nullptr, n, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (page == MAP_FAILED) {
        std::fprintf(stderr, "CASE unmap_fail setup page\n");
        return 2;
    }
    backing.unmap(MapleRuntime::to_zaddress_unsafe(reinterpret_cast<uintptr_t>(page)), 0);
    std::fprintf(stderr, "CASE unmap_fail returned\n");
    return 0;
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s ctor_ok|ctor_fail|map_ok|map_fail|unmap_ok|unmap_fail\n", argv[0]);
        return 2;
    }
    if (std::strcmp(argv[1], "ctor_ok") == 0) {
        return ctor_ok();
    }
    if (std::strcmp(argv[1], "ctor_fail") == 0) {
        return ctor_fail();
    }
    if (std::strcmp(argv[1], "map_ok") == 0) {
        return map_ok();
    }
    if (std::strcmp(argv[1], "map_fail") == 0) {
        return map_fail();
    }
    if (std::strcmp(argv[1], "unmap_ok") == 0) {
        return unmap_ok();
    }
    if (std::strcmp(argv[1], "unmap_fail") == 0) {
        return unmap_fail();
    }
    std::fprintf(stderr, "unknown case %s\n", argv[1]);
    return 2;
}
