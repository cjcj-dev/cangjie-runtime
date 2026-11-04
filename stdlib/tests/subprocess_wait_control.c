/* Test-only native control. It is linked into the subprocess test executable
 * and is not part of std.process or any SDK archive. */

#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t g_received_term = 0;

static void receive_term(int signal_number)
{
    (void)signal_number;
    g_received_term = 1;
}

int32_t CJ_TEST_SubprocessStart(void)
{
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        struct sigaction action = {0};
        action.sa_handler = receive_term;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGTERM, &action, NULL) != 0) {
            _exit(127);
        }
        while (!g_received_term) {
            pause();
        }
        // Keep the child alive after the successful kill so a real
        // waitpid(WNOHANG) probe observes running state at least once.
        usleep(200000);
        _exit(0);
    }
    return (int32_t)pid;
}

int32_t CJ_TEST_SubprocessKillFailure(int32_t pid)
{
    errno = 0;
    int result = kill((pid_t)pid, 999);
    return result == -1 ? -1 : 0;
}

int32_t CJ_TEST_SubprocessKillRetry(int32_t pid)
{
    return kill((pid_t)pid, SIGTERM) == 0 ? 0 : -1;
}

int32_t CJ_TEST_SubprocessPoll(int32_t pid)
{
    int status = 0;
    pid_t result = waitpid((pid_t)pid, &status, WNOHANG);
    if (result == 0 || (result < 0 && errno == EINTR)) {
        return 0;
    }
    if (result == (pid_t)pid) {
        return 1;
    }
    return -1;
}

void CJ_TEST_SubprocessCleanup(int32_t pid)
{
    if (pid <= 0) {
        return;
    }
    (void)kill((pid_t)pid, SIGKILL);
    while (waitpid((pid_t)pid, NULL, 0) < 0 && errno == EINTR) {
    }
}
