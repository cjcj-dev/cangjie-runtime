// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.

import std.process.*
import std.fs.*

main(): Int64 {
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

    // Concurrent waiters share one native reap and observe one cached status.
    let concurrent = launch("/bin/sh", ["-c", "sleep 0.02; exit 9"],
        stdOut: Discard, stdErr: Discard)
    let first: Future<Int64> = spawn {=> concurrent.wait()}
    let second: Future<Int64> = spawn {=> concurrent.wait()}
    if (first.get() != 9 || second.get() != 9) {
        return 3
    }

    // execute() owns the temporary SubProcess, so its timeout path must kill
    // and reap the child before returning. A post-timeout shell action must
    // never run.
    let marker = Path("/tmp/cj_subprocess_timeout_marker")
    if (exists(marker)) {
        remove(marker)
    }
    try {
        execute("/bin/sh", ["-c", "sleep 5; echo late > /tmp/cj_subprocess_timeout_marker"],
            stdOut: Discard, stdErr: Discard, timeout: Duration.millisecond * 10)
        return 4
    } catch (_: TimeoutException) {
    }
    sleep(Duration.millisecond * 100)
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
