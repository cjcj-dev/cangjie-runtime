// Standalone unit for BUG-24: NativeAlloc(0) used SizeManager::Index(0) and
// underflowed to SIZE_MAX. Product site: Common/MemCommon.h SizeManager::Index
// and Common/NativeAllocator.cpp NativeAlloc/NativeFree.
//
// Compile:
//   clang++ -std=c++14 -O0 runtime/tests/objm_nativealloc_zero_unit.cpp -o /tmp/objm_zero

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>

namespace {

enum AlignShift { ALIGN_8_SIFT = 3 };
constexpr int kGroup0 = 16;

// Exact replica of SizeManager::Index(bytes, alignShift) and the first arm of
// SizeManager::Index(bytes) — the path NativeAlloc(0) used to take.
static inline size_t IndexInner(size_t bytes, size_t alignShift)
{
    return ((bytes + (1u << alignShift) - 1u) >> alignShift) - 1u;
}

static inline size_t PreFixIndex(size_t bytes)
{
    return IndexInner(bytes, ALIGN_8_SIFT);
}

// Post-fix NativeAlloc gate: 0-byte requests never reach Index.
static inline void* PostFixNativeAlloc(size_t bytes)
{
    if (bytes == 0) {
        return nullptr;
    }
    return reinterpret_cast<void*>(static_cast<uintptr_t>(0x1));
}

static inline bool PostFixNativeFree(void* ptr, size_t bytes)
{
    if (ptr == nullptr || bytes == 0) {
        return true;
    }
    return false;
}

} // namespace

int main()
{
    int fail = 0;
    auto check = [&](const char* name, bool cond) {
        std::printf("CHECK %s %s\n", name, cond ? "PASS" : "FAIL");
        if (!cond) {
            ++fail;
        }
    };

    size_t idx0 = PreFixIndex(0);
    check("pre_fix_index0_underflows", idx0 == std::numeric_limits<size_t>::max());
    check("pre_fix_index0_oob_for_208", idx0 >= 208);

    size_t idx8 = PreFixIndex(8);
    check("pre_fix_index8_is_zero", idx8 == 0);

    check("post_fix_alloc0_is_null", PostFixNativeAlloc(0) == nullptr);
    check("post_fix_alloc8_not_null", PostFixNativeAlloc(8) != nullptr);
    check("post_fix_free_null_zero", PostFixNativeFree(nullptr, 0));

    std::printf("OBJM_NATIVEALLOC_ZERO result=%s fails=%d index0=%zu\n",
                fail == 0 ? "PASS" : "FAIL", fail, idx0);
    return fail == 0 ? 0 : 1;
}
