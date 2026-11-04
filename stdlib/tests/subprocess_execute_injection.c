/* Test-only link-time wrappers for the real std.process native boundary.
 * The executable is linked with --wrap for these two CJ_OS symbols; this file
 * is never part of std.process or an SDK archive. */

#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <time.h>
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
// Only this gate crosses the execute/observer managed-task boundary.
// 0: not in cleanup; 1: cleanup returned poll=0; 2: observer acknowledged;
// 3: execute observed acknowledgement before calling the real native reap.
static _Atomic int32_t g_progress_gate = 0;
static int32_t g_progress_before_reap = 0;
static int32_t g_retry_one = 0;
static int32_t g_exit_ready = 0;
static int32_t g_child_reaped = 0;
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
    atomic_store_explicit(&g_progress_gate, 0, memory_order_relaxed);
    g_retry_one = 0;
    g_exit_ready = 0;
    g_child_reaped = 0;
    g_progress_before_reap = 0;
    g_injected_cleanup = 0;
}

int32_t CJ_TEST_WrapAcknowledgeWaiting(void)
{
    int32_t expected = 1;
    return atomic_compare_exchange_strong_explicit(&g_progress_gate, &expected, 2,
        memory_order_acq_rel, memory_order_acquire) ? 1 : 0;
}

int32_t CJ_TEST_WrapRetryOne(void) { return g_retry_one; }
int32_t CJ_TEST_WrapExitReady(void) { return g_exit_ready; }
int32_t CJ_TEST_WrapChildReaped(void) { return g_child_reaped; }

int32_t __wrap_CJ_OS_Terminate(int32_t pid, bool force)
{
    g_last_pid = pid;
    g_terminate_calls++;
    int32_t result = __real_CJ_OS_Terminate(pid, force);
    if (g_terminate_calls == 1 && result == 0) {
        g_injected_cleanup = 1;
        if (g_mode == 3) {
            // Leave the real child waitable, but make its exit deterministic
            // before reporting kill failure. Only the product retry consumes it.
            const struct timespec pause = {0, 1000000};
            for (int i = 0; i < 1000; ++i) {
                siginfo_t info = {0};
                int rc = waitid(P_PID, (id_t)pid, &info, WEXITED | WNOHANG | WNOWAIT);
                if (rc == 0 && info.si_pid == pid) {
                    g_exit_ready = 1;
                    break;
                }
                if (rc < 0 && errno != EINTR) break;
                nanosleep(&pause, NULL);
            }
        }
        if (g_mode == 1 || g_mode == 3) {
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

    if (g_injected_cleanup && g_mode == 4) {
        if (g_poll_zero_calls == 0) {
            g_poll_zero_calls++;
            return 0;
        }
        if (g_poll_zero_calls == 1) {
            // Reaching a second poll proves product sleep/yield ran after
            // the first zero. Only now can the managed observer acknowledge.
            g_poll_zero_calls++;
            atomic_store_explicit(&g_progress_gate, 1, memory_order_release);
            return 0;
        }
        int32_t gate = atomic_load_explicit(&g_progress_gate, memory_order_acquire);
        if (gate == 1) {
            // Never block the native call waiting for a managed task. Return
            // to the product's sleep/yield until that task acknowledges.
            if (++g_poll_zero_calls >= 32) return -1;
            return 0;
        }
        if (gate == 2) {
            g_progress_before_reap = 1;
            atomic_store_explicit(&g_progress_gate, 3, memory_order_release);
        } else if (gate != 3) {
            return -1;
        }
        // This is strictly after the acquire of the observer's acknowledgement
        // and strictly before the first real post-kill native reap call.
    }

    int32_t result = __real_CJ_OS_WaitSubProcessExitNonBlocking(pid, exitCode);
    if (result == 1) g_child_reaped = 1;
    if (g_injected_cleanup && g_mode == 3 && result == 1) g_retry_one++;
    if (g_injected_cleanup && g_mode == 2 && result == 1) {
        g_reap_negative_seen = 1;
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
    if (g_last_pid <= 0 || g_child_reaped) {
        return;
    }
    (void)kill((pid_t)g_last_pid, SIGKILL);
    while (waitpid((pid_t)g_last_pid, NULL, 0) < 0 && errno == EINTR) {
    }
}
