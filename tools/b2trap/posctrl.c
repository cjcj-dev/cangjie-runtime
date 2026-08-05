#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

volatile uint64_t g_slot __attribute__((aligned(8)));
volatile uint64_t g_slot2 __attribute__((aligned(8)));

__attribute__((noinline)) void writer_main(uint64_t v) { g_slot = v; }
__attribute__((noinline)) void writer_thread(uint64_t v) { g_slot = v; }

static void *th(void *p) { (void)p; writer_thread(0xdeadbeef); return 0; }

int main(int argc, char **argv) {
    printf("SLOT=%p SLOT2=%p\n", (void*)&g_slot, (void*)&g_slot2);
    fflush(stdout);
    writer_main(0x1111);
    pthread_t t; pthread_create(&t, 0, th, 0); pthread_join(t, 0);
    writer_main(0x2222);
    if (argc > 1 && !strcmp(argv[1], "abort")) { abort(); }
    printf("POSCTRL_OK\n");
    return 0;
}
