// Standalone unit for BUG-21: InstanceFieldInfo GetValue/SetValue used to
// feed a null malloc buffer into ReadStruct. Product site:
// ObjectModel/FieldInfo.cpp InstanceFieldInfo::{GetValue,SetValue}.
// The product now matches GetAnnotations / StructLikeToAny: check malloc,
// then ExceptionManager::OutOfMemory() and do not enter ReadStruct.
//
// Compile:
//   clang++ -std=c++14 -O0 runtime/tests/objm_fieldinfo_malloc_unit.cpp -o /tmp/objm_malloc

#include <cstddef>
#include <cstdio>

namespace {

enum Path { PATH_READSTRUCT, PATH_OOM };

Path PreFixGetOrSet(void* mallocResult, size_t fieldSize)
{
    if (fieldSize == 0) {
        return PATH_OOM;
    }
    void* tmp = mallocResult;
    (void)tmp;
    // Pre-fix: no null check; ReadStruct(tmp, ...) always runs.
    return PATH_READSTRUCT;
}

Path PostFixGetOrSet(void* mallocResult, size_t fieldSize)
{
    if (fieldSize == 0) {
        return PATH_OOM;
    }
    void* tmp = mallocResult;
    if (tmp == nullptr) {
        return PATH_OOM;
    }
    return PATH_READSTRUCT;
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

    check("pre_null_malloc_enters_readstruct",
          PreFixGetOrSet(nullptr, 16) == PATH_READSTRUCT);
    check("post_null_malloc_reports_oom",
          PostFixGetOrSet(nullptr, 16) == PATH_OOM);
    check("post_ok_malloc_still_copies",
          PostFixGetOrSet(reinterpret_cast<void*>(0x10), 16) == PATH_READSTRUCT);
    check("post_zero_size_skips_malloc",
          PostFixGetOrSet(nullptr, 0) == PATH_OOM);

    std::printf("OBJM_FIELDINFO_MALLOC result=%s fails=%d\n", fail == 0 ? "PASS" : "FAIL", fail);
    return fail == 0 ? 0 : 1;
}
