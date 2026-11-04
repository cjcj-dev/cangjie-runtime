/* Minimal linker-section regression. This does not qualify managed unwinding. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern char g_runtimeStaticStart, g_runtimeStaticEnd;
__attribute__((noinline, section(".text.MonitorScopeControl")))
void MonitorScopeControl(void) { __asm__ volatile(""); }
__attribute__((noinline, section(".text._CNuser_UserMonitorScope")))
void UserMonitorScope(void) { __asm__ volatile(""); }
__attribute__((noinline, section(".text._CNuser_PlainScope")))
void PlainScope(void) { __asm__ volatile(""); }
static int in_runtime(void (*fn)(void))
{
    uintptr_t pc = (uintptr_t)fn;
    return (uintptr_t)&g_runtimeStaticStart <= pc && pc < (uintptr_t)&g_runtimeStaticEnd;
}
int main(int argc, char **argv)
{
    int native = in_runtime(MonitorScopeControl), user = in_runtime(UserMonitorScope);
    int plain = in_runtime(PlainScope);
    printf("MONITOR_SCOPE_ASSERT native=%d user=%d plain=%d start=%p end=%p\n",
           native, user, plain, (void*)&g_runtimeStaticStart, (void*)&g_runtimeStaticEnd);
    if (!native || plain) return 42;
    if (argc == 2 && strcmp(argv[1], "control") == 0) return 0;
    return user ? 41 : 0;
}
