// Standalone unit for BUG-23: overlong ULEB used to shift a uint64 by 70.
// Does not link libcangjie-runtime. Product site: ObjectModel/MFunc.cpp ULEBDecodeSingleStr.
//
// Compile:
//   clang++ -std=c++14 -O0 -fsanitize=undefined runtime/tests/objm_uleb_overshift_unit.cpp -o /tmp/objm_uleb

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr uint8_t kIntValueBits = 7;
constexpr uint32_t kMaxShift = 63;

// Pre-fix decoder: records the largest shift actually applied. The product
// used to do `uint64 << shift` with no cap, so 11 continuation bytes reach 70.
uint64_t PreFixULEB(std::vector<uint8_t> bytes, uint32_t* maxShift)
{
    uint64_t result = 0;
    uint32_t shift = 0;
    *maxShift = 0;
    for (auto byte : bytes) {
        if (shift > *maxShift) {
            *maxShift = shift;
        }
        result |= static_cast<uint64_t>(byte & 0x7f) << (shift > 63 ? 0 : shift);
        if ((byte & 0x80) == 0) {
            break;
        }
        shift += kIntValueBits;
    }
    if (shift > *maxShift) {
        *maxShift = shift;
    }
    return result;
}

// Post-fix decoder: same control flow as MFunc.cpp after the BUG-23 fix.
uint64_t PostFixULEB(std::vector<uint8_t>& bytes, bool* rejected)
{
    uint64_t result = 0;
    uint32_t shift = 0;
    size_t consumed = 0;
    *rejected = false;
    for (auto byte : bytes) {
        if (shift > kMaxShift) {
            *rejected = true;
            bytes.clear();
            return 0;
        }
        result |= static_cast<uint64_t>(byte & 0x7f) << shift;
        ++consumed;
        if ((byte & 0x80) == 0) {
            bytes.erase(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(consumed));
            return result;
        }
        shift += kIntValueBits;
    }
    bytes.clear();
    *rejected = true;
    return 0;
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

    std::vector<uint8_t> overlong(12, 0x81);
    overlong.back() = 0x00;

    uint32_t preMax = 0;
    (void)PreFixULEB(overlong, &preMax);
    check("pre_fix_shift_reaches_70", preMax >= 70);

    std::vector<uint8_t> postIn = overlong;
    bool rejected = false;
    uint64_t post = PostFixULEB(postIn, &rejected);
    check("post_fix_rejects_overlong", rejected && post == 0);
    check("post_fix_consumes_input", postIn.empty());

    std::vector<uint8_t> one{ 0x05 };
    bool oneRejected = true;
    uint64_t oneVal = PostFixULEB(one, &oneRejected);
    check("post_fix_keeps_one_byte", oneVal == 5 && !oneRejected && one.empty());

    std::vector<uint8_t> two{ 0x81, 0x00 };
    bool twoRejected = true;
    uint64_t twoVal = PostFixULEB(two, &twoRejected);
    check("post_fix_keeps_two_byte", twoVal == 1 && !twoRejected && two.empty());

    std::printf("OBJM_ULEB_OVERSHIFT result=%s fails=%d pre_max_shift=%u\n",
                fail == 0 ? "PASS" : "FAIL", fail, preMax);
    return fail == 0 ? 0 : 1;
}
