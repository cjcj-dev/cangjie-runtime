// Native holder survives a real Cangjie callback that grows the same stack.
#include <cstdint>
extern "C" int64_t StableStackCallback(int64_t (*callback)()) {
    volatile uint64_t words[8] = {11,13,17,19,23,29,31,37};
    volatile uint64_t* const held = &words[4];
    const int64_t result = callback();
    return *held == 23 && words[7] == 37 ? result : -99;
}
