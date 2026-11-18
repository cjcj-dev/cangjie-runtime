// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.

// Native half of main.cj. The holder keeps the raw pointer of an acquired
// Array<Int64> and blocks here; each stage change is published under a mutex,
// so a Cangjie reader that returns from rd_wait_stage happens-after the write.
#include <pthread.h>
#include <stdint.h>

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int64_t g_stage = 0;

static void set_stage(int64_t s)
{
    pthread_mutex_lock(&g_mu);
    g_stage = s;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

static void wait_stage(int64_t s)
{
    pthread_mutex_lock(&g_mu);
    while (g_stage < s) {
        pthread_cond_wait(&g_cv, &g_mu);
    }
    pthread_mutex_unlock(&g_mu);
}

// Writes s1, waits until Cangjie has checked it and collected, writes s2 through
// the same pointer, then waits until Cangjie has checked s2. Returns the pointer.
int64_t rd_hold(int64_t* p, int64_t idx, int64_t s1, int64_t s2)
{
    p[idx] = s1;
    set_stage(1);
    wait_stage(2);
    p[idx] = s2;
    set_stage(3);
    wait_stage(4);
    return (int64_t)(intptr_t)p;
}

void rd_wait_stage(int64_t s)
{
    wait_stage(s);
}

void rd_set_stage(int64_t s)
{
    set_stage(s);
}
