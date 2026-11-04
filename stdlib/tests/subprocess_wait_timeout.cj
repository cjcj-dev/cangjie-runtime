// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.

import std.process.*
import std.fs.*
import std.time.*

@When[os == "Linux"]
foreign {
    func CJ_TEST_SubprocessStart(): Int32
    func CJ_TEST_SubprocessKillFailure(pid: Int32): Int32
    func CJ_TEST_SubprocessKillRetry(pid: Int32): Int32
    func CJ_TEST_SubprocessPoll(pid: Int32): Int32
    func CJ_TEST_SubprocessCleanup(pid: Int32): Unit
}

@When[os == "Linux"]
func runNativeWaitControl(): Int64 {
    let pid = unsafe { CJ_TEST_SubprocessStart() }
    if (pid <= 0) {
        return 10
    }
    if (unsafe { CJ_TEST_SubprocessKillFailure(pid) } != -1) {
        unsafe { CJ_TEST_SubprocessCleanup(pid) }
        return 11
    }
    if (unsafe { CJ_TEST_SubprocessKillRetry(pid) } != 0) {
        unsafe { CJ_TEST_SubprocessCleanup(pid) }
        return 12
    }

    let progressStart = MonoTime.now()
    let progress: Future<Int64> = spawn {=>
        sleep(Duration.millisecond * 20)
        return 1
    }
    var sawPollZero = false
    while (true) {
        let pollResult = unsafe { CJ_TEST_SubprocessPoll(pid) }
        if (pollResult == 0) {
            sawPollZero = true
            sleep(Duration.millisecond)
            continue
        }
        if (pollResult < 0) {
            unsafe { CJ_TEST_SubprocessCleanup(pid) }
            return 13
        }
        break
    }
    if (!sawPollZero || progress.get() != 1 ||
        MonoTime.now() - progressStart > Duration.millisecond * 500) {
        return 14
    }
    if (unsafe { CJ_TEST_SubprocessPoll(pid) } != -1) {
        return 15
    }
    return 0
}

@When[os != "Linux"]
func runNativeWaitControl(): Int64 {
    return 0
}

main(): Int64 {
    let nativeControlResult = runNativeWaitControl()
    if (nativeControlResult != 0) {
        return nativeControlResult
    }

    // A timed poll must leave the child waitable, and every later wait must
    // return the same status after the first caller reaps it.
    let process = launch("/bin/sh", ["-c", "sleep 0.05; exit 7"],
        stdOut: Discard, stdErr: Discard)
    try {
        process.wait(timeout: Duration.millisecond * 1)
        return 1
    } catch (_: TimeoutException) {
    }
    if (process.wait() != 7 || process.wait() != 7) {
        return 2
    }
    // A reap-complete process must propagate a failed terminate instead of
    // treating the platform return value as a successful kill.
    try {
        process.terminate(force: true)
        return 7
    } catch (_: ProcessException) {
    }

    // Timeout cleanup must yield between reap probes. A sibling Future should
    // make progress while the timed-out child is being killed and collected.
    let progressStart = MonoTime.now()
    let progress: Future<Int64> = spawn {=>
        sleep(Duration.millisecond * 20)
        return 1
    }
    try {
        execute("/bin/sh", ["-c", "sleep 1"],
            stdOut: Discard, stdErr: Discard, timeout: Duration.millisecond * 10)
        return 8
    } catch (_: TimeoutException) {
    }
    if (progress.get() != 1 || MonoTime.now() - progressStart > Duration.millisecond * 500) {
        return 9
    }

    // Concurrent waiters share one native reap and observe one cached status.
    let concurrent = launch("/bin/sh", ["-c", "printf concurrent; printf err >&2; exit 9"],
        stdOut: Pipe, stdErr: Pipe)
    let first: Future<Int64> = spawn {=> concurrent.wait()}
    let second: Future<Int64> = spawn {=> concurrent.wait()}
    if (first.get() != 9 || second.get() != 9) {
        return 3
    }

    // execute() owns the temporary SubProcess, so its timeout path must kill
    // and reap the child before returning. The short delayed write makes a
    // failed kill observable within this test instead of checking too early.
    let marker = Path("/tmp/cj_subprocess_timeout_marker")
    if (exists(marker)) {
        remove(marker)
    }
    try {
        execute("/bin/sh", ["-c", "sleep 0.2; echo late > /tmp/cj_subprocess_timeout_marker"],
            stdOut: Discard, stdErr: Discard, timeout: Duration.millisecond * 10)
        return 4
    } catch (_: TimeoutException) {
    }
    sleep(Duration.millisecond * 400)
    if (exists(marker)) {
        remove(marker)
        return 5
    }

    // The public output consumer still drains both pipes before waiting.
    let (code, output, error) = executeWithOutput("/bin/sh",
        ["-c", "printf out; printf err >&2"])
    if (code != 0 || String.fromUtf8(output) != "out" || String.fromUtf8(error) != "err") {
        return 6
    }
    println("SUBPROCESS_WAIT_TIMEOUT_PASS")
    return 0
}
