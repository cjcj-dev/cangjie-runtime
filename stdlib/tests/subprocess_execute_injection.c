/* Test-only link-time wrappers for the real std.process native boundary.
 * The executable is linked with --wrap for these two CJ_OS symbols; this file
 * is never part of std.process or an SDK archive. */

#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern int32_t __real_CJ_OS_Terminate(int32_t pid, bool force);
extern int32_t __real_CJ_OS_WaitSubProcessExitNonBlocking(int32_t pid, int64_t* exitCode);

static int32_t g_last_pid = -1;
static int32_t g_terminate_calls = 0;
static int32_t g_poll_calls = 0;
static int32_t g_poll_zero_calls = 0;
static int32_t g_reap_negative_seen = 0;
static int32_t g_terminate_failure_seen = 0;
static int32_t g_progress_marked = 0;
static int32_t g_progress_before_reap = 0;
static int32_t g_mode = 0;
static int32_t g_injected_cleanup = 0;

void CJ_TEST_WrapReset(int32_t mode)
{
    g_mode = mode;
    g_last_pid = -1;
    g_terminate_calls = 0;
    g_poll_calls = 0;
    g_poll_zero_calls = 0;
    g_reap_negative_seen = 0;
    g_terminate_failure_seen = 0;
    g_progress_marked = 0;
    g_progress_before_reap = 0;
    g_injected_cleanup = 0;
}

void CJ_TEST_WrapMarkProgress(void)
{
    g_progress_marked = 1;
}

int32_t __wrap_CJ_OS_Terminate(int32_t pid, bool force)
{
    g_last_pid = pid;
    g_terminate_calls++;
    int32_t result = __real_CJ_OS_Terminate(pid, force);
    if (g_terminate_calls == 1 && result == 0) {
        g_injected_cleanup = 1;
        if (g_mode == 1) {
            // The real kill has happened; report one native failure to
            // exercise executeImpl -> cleanup -> poll retry on the product
            // path.
            g_terminate_failure_seen = 1;
            return -1;
        }
    }
    return result;
}

int32_t __wrap_CJ_OS_WaitSubProcessExitNonBlocking(int32_t pid, int64_t* exitCode)
{
    g_poll_calls++;
    if (g_injected_cleanup && g_mode == 1 && g_poll_zero_calls < 1) {
        g_poll_zero_calls++;
        return 0;
    }
    if (g_injected_cleanup && g_mode == 2 && g_poll_zero_calls < 50) {
        // Keep the child unreaped while the product's managed sleep yields.
        g_poll_zero_calls++;
        return 0;
    }

    int32_t result = __real_CJ_OS_WaitSubProcessExitNonBlocking(pid, exitCode);
    if (g_injected_cleanup && g_mode == 2 && result == 1) {
        g_reap_negative_seen = 1;
        g_progress_before_reap = g_progress_marked;
        // The real native wait has consumed the child; expose a negative
        // status to the product so its error propagation is exercised.
        return -1;
    }
    return result;
}

int32_t CJ_TEST_WrapTerminateFailureSeen(void) { return g_terminate_failure_seen; }
int32_t CJ_TEST_WrapPollZeroCalls(void) { return g_poll_zero_calls; }
int32_t CJ_TEST_WrapReapNegativeSeen(void) { return g_reap_negative_seen; }
int32_t CJ_TEST_WrapProgressBeforeReap(void) { return g_progress_before_reap; }

void CJ_TEST_WrapCleanup(void)
{
    if (g_last_pid <= 0 || g_reap_negative_seen) {
        return;
    }
    (void)kill((pid_t)g_last_pid, SIGKILL);
    while (waitpid((pid_t)g_last_pid, NULL, 0) < 0 && errno == EINTR) {
    }
}
