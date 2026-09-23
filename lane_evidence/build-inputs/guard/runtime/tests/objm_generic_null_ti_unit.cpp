// Standalone unit for BUG-22: constrained generic reflection still deref'd a
// null TypeInfo after GetActualTypeFromGenericType failed. Product site:
// ObjectModel/MethodInfo.cpp CheckMethodActualArgs / CheckGenericConstraint /
// GetActualTypeFromGenericType.
//
// Compile:
//   clang++ -std=c++14 -O0 runtime/tests/objm_generic_null_ti_unit.cpp -o /tmp/objm_generic

#include <cstdint>
#include <cstdio>

namespace {

struct FakeRaw {
    uint64_t len;
    void* data[4];
};

struct FakeArray {
    FakeRaw* rawPtr;
};

// Pre-fix: after resolving ti, go straight into constraint check and call
// ti->IsSubType. A null / short / missing genericArgs array produces a crash.
enum PreFixFate { PRE_OK, PRE_NULL_DEREF, PRE_OOB };

PreFixFate PreFixCheck(uint16_t genericCnt, uint32_t constraintCnt, FakeArray* genericArgs)
{
    if (genericCnt == 0) {
        return PRE_OK;
    }
    // GetActualTypeFromGenericType used to walk idx < genericCnt and read
    // genericArgs->rawPtr->data[idx] with no length check.
    if (genericArgs == nullptr) {
        return PRE_NULL_DEREF;
    }
    if (genericArgs->rawPtr == nullptr) {
        return PRE_NULL_DEREF;
    }
    for (uint16_t idx = 0; idx < genericCnt; ++idx) {
        if (idx >= genericArgs->rawPtr->len) {
            return PRE_OOB;
        }
        void* ti = genericArgs->rawPtr->data[idx];
        if (constraintCnt > 0 && ti == nullptr) {
            return PRE_NULL_DEREF;
        }
    }
    return PRE_OK;
}

// Post-fix: same cases must return false without touching a null TI.
bool PostFixCheck(uint16_t genericCnt, uint32_t constraintCnt, FakeArray* genericArgs)
{
    if (genericCnt > 0) {
        if (genericArgs == nullptr) {
            return false;
        }
        if (genericArgs->rawPtr == nullptr || genericArgs->rawPtr->len < genericCnt) {
            return false;
        }
    }
    for (uint16_t idx = 0; idx < genericCnt; ++idx) {
        void* ti = genericArgs->rawPtr->data[idx];
        if (constraintCnt > 0 && ti == nullptr) {
            return false;
        }
    }
    return true;
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

    FakeRaw empty{ 0, { nullptr, nullptr, nullptr, nullptr } };
    FakeRaw shortArr{ 0, { nullptr, nullptr, nullptr, nullptr } };
    FakeRaw oneNull{ 1, { nullptr, nullptr, nullptr, nullptr } };
    FakeRaw oneOk{ 1, { reinterpret_cast<void*>(0x10), nullptr, nullptr, nullptr } };
    FakeArray aEmpty{ &empty };
    FakeArray aShort{ &shortArr };
    FakeArray aOneNull{ &oneNull };
    FakeArray aOneOk{ &oneOk };

    check("pre_null_args_deref", PreFixCheck(1, 1, nullptr) == PRE_NULL_DEREF);
    check("pre_empty_oob", PreFixCheck(1, 1, &aEmpty) == PRE_OOB);
    check("pre_short_oob", PreFixCheck(1, 1, &aShort) == PRE_OOB);
    check("pre_one_null_deref", PreFixCheck(1, 1, &aOneNull) == PRE_NULL_DEREF);

    check("post_null_args_false", !PostFixCheck(1, 1, nullptr));
    check("post_empty_false", !PostFixCheck(1, 1, &aEmpty));
    check("post_short_false", !PostFixCheck(1, 1, &aShort));
    check("post_one_null_false", !PostFixCheck(1, 1, &aOneNull));
    check("post_one_ok_true", PostFixCheck(1, 1, &aOneOk));
    check("post_no_generic_true", PostFixCheck(0, 0, nullptr));

    std::printf("OBJM_GENERIC_NULL_TI result=%s fails=%d\n", fail == 0 ? "PASS" : "FAIL", fail);
    return fail == 0 ? 0 : 1;
}
