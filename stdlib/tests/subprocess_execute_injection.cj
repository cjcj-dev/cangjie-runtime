// Test-only executable. It drives the public execute() entry while link-time
// wrappers observe the real CJ_OS cleanup calls and their ordering.

import std.process.*
import std.time.*

@When[os == "Linux"]
foreign {
    func CJ_TEST_WrapReset(mode: Int32): Unit
    func CJ_TEST_WrapMarkProgress(): Unit
    func CJ_TEST_WrapTerminateFailureSeen(): Int32
    func CJ_TEST_WrapPollZeroCalls(): Int32
    func CJ_TEST_WrapReapNegativeSeen(): Int32
    func CJ_TEST_WrapProgressBeforeReap(): Int32
    func CJ_TEST_WrapCleanup(): Unit
}

@When[os == "Linux"]
func runKillFailureControl(): Int64 {
    unsafe { CJ_TEST_WrapReset(1) }
    try {
        execute("/bin/sh", ["-c", "sleep 1"],
            stdOut: Discard, stdErr: Discard, timeout: Duration.millisecond * 10)
        unsafe { CJ_TEST_WrapCleanup() }
        return 10
    } catch (_: ProcessException) {
    }

    let terminateFailure = unsafe { CJ_TEST_WrapTerminateFailureSeen() }
    let pollZeroCalls = unsafe { CJ_TEST_WrapPollZeroCalls() }
    unsafe { CJ_TEST_WrapCleanup() }
    if (terminateFailure != 1 || pollZeroCalls != 1) {
        return 11
    }
    return 0
}

@When[os == "Linux"]
func runReapNegativeProgressControl(): Int64 {
    unsafe { CJ_TEST_WrapReset(2) }
    let progress: Future<Int64> = spawn {=>
        sleep(Duration.millisecond * 20)
        unsafe { CJ_TEST_WrapMarkProgress() }
        return 1
    }

    try {
        execute("/bin/sh", ["-c", "sleep 1"],
            stdOut: Discard, stdErr: Discard, timeout: Duration.millisecond * 10)
        unsafe { CJ_TEST_WrapCleanup() }
        return 12
    } catch (_: ProcessException) {
    }

    let progressResult = progress.get()
    let pollZeroCalls = unsafe { CJ_TEST_WrapPollZeroCalls() }
    let reapNegative = unsafe { CJ_TEST_WrapReapNegativeSeen() }
    let progressBeforeReap = unsafe { CJ_TEST_WrapProgressBeforeReap() }
    println("INJECTION_FLAGS progress=${progressResult} poll0=${pollZeroCalls} reapNeg=${reapNegative} progressBeforeReap=${progressBeforeReap}")
    if (progressResult != 1 || pollZeroCalls < 50 || reapNegative != 1 || progressBeforeReap != 1) {
        unsafe { CJ_TEST_WrapCleanup() }
        return 13
    }
    return 0
}

@When[os != "Linux"]
func runKillFailureControl(): Int64 { return 0 }

@When[os != "Linux"]
func runReapNegativeProgressControl(): Int64 { return 0 }

main(): Int64 {
    let killFailureResult = runKillFailureControl()
    if (killFailureResult != 0) {
        return killFailureResult
    }
    runReapNegativeProgressControl()
}
