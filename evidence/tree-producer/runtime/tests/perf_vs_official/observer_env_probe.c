#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void print_env(const char* name)
{
    const char* value = getenv(name);
    printf("%s=%s\n", name, value == NULL ? "UNSET" : value);
}

int main(void)
{
    usleep(20000);
    print_env("MRT_LOG_LEVEL");
    print_env("MRT_LOG_PATH");
    print_env("MRT_REPORT");
    print_env("MRT_GC_LOG");
    puts("OBSERVER_ENV_OK");
    return 0;
}
