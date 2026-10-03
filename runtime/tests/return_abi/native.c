#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct { uint64_t first, second; } Pair;
static int target(const char *name, int ok) {
    printf("RETURN_ABI_TARGET %s ok=%d\n", name, ok);
    fflush(stdout);
    return !ok;
}

int64_t RunUInt(uint64_t (*callback)(uint64_t)) {
    uintptr_t begin = (uintptr_t)dlsym(RTLD_DEFAULT, "CJ_MCC_N2CStub");
    uintptr_t end = (uintptr_t)dlsym(RTLD_DEFAULT, "ExecuteCangjieStub");
    if (!begin || end <= begin) {
        puts("RETURN_ABI_INPUT_MISSING stub bounds");
        return 100;
    }
    const uint64_t values[] = {begin, begin + 1, end - 1, begin - 1, end, end + 1,
                              0, UINT64_MAX, UINT64_C(0x123456789abcdef0)};
    const char *names[] = {"stub_begin", "stub_inside", "stub_last", "below",
                          "end", "above", "zero", "max", "integer_control"};
    int64_t failures = 0;
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        uint64_t actual = callback(values[i]);
        printf("RETURN_ABI_VALUE %s expected=%016llx actual=%016llx\n", names[i],
               (unsigned long long)values[i], (unsigned long long)actual);
        failures += target(names[i], actual == values[i]);
    }
    return failures;
}

int64_t RunPair(Pair (*callback)(Pair)) {
    Pair expected = {UINT64_C(0x1020304050607080), UINT64_C(0xfedcba9876543210)};
    Pair actual = callback(expected);
    return target("pair_control", actual.first == expected.first && actual.second == expected.second);
}

int64_t RunFloat(float (*single)(float), double (*wide)(double)) {
    float f = -1.25f;
    double d = 123456.75;
    float af = single(f);
    double ad = wide(d);
    return target("float_control", memcmp(&f, &af, sizeof(f)) == 0) +
           target("double_control", memcmp(&d, &ad, sizeof(d)) == 0);
}

void CrossPending(void (*callback)(void)) {
    callback();
    // The callback's ordinary result is deliberately not consumed: the real
    // C2N leave checks ExceptionWrapper and resumes the managed handler.
    puts("RETURN_ABI_PENDING_NATIVE_RETURN_REACHED");
    fflush(stdout);
}
