// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.

// Native half of main.cj. Writes through the acquired pointer, blocks legally
// for `usec` microseconds while the raw data is held, then writes again through
// the same pointer.
#include <stdint.h>
#include <time.h>

int64_t lw_fill(int64_t* p, int64_t n, int64_t v, int64_t usec)
{
    for (int64_t i = 0; i < n; ++i) {
        p[i] = v + i;
    }
    struct timespec ts = { (time_t)(usec / 1000000), (long)((usec % 1000000) * 1000) };
    nanosleep(&ts, 0);
    for (int64_t i = 0; i < n; ++i) {
        p[i] = v + 2 * i;
    }
    return n;
}
