// Linux test observer: count FIFO bytes without consuming a jobserver token.
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    char fifo[4096] = {0};
    int fd = -1;
    const struct timespec tick = {0, 10000000};
    for (int sample = 0; sample < 60000; ++sample) {
        if (fd < 0) {
            DIR *dir = opendir(argv[1]);
            if (dir) {
                struct dirent *entry;
                while ((entry = readdir(dir)) != NULL) {
                    if (strncmp(entry->d_name, ".cangjie-jobserver-", 19) != 0) continue;
                    int n = snprintf(fifo, sizeof(fifo), "%s/%s/fifo", argv[1], entry->d_name);
                    if (n > 0 && (size_t)n < sizeof(fifo)) fd = open(fifo, O_RDWR | O_NONBLOCK);
                    if (fd >= 0) break;
                }
                closedir(dir);
            }
        } else {
            if (access(fifo, F_OK) != 0) { close(fd); return 0; }
            int available = -1;
            struct timespec now;
            if (ioctl(fd, FIONREAD, &available) != 0 || clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 3;
            uint64_t ns = (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
            printf("%" PRIu64 ",%d\n", ns, available);
        }
        nanosleep(&tick, NULL);
    }
    if (fd >= 0) close(fd);
    return 4;
}
