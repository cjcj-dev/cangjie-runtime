// Test-only executable. It drives the public execute() entry while link-time
// wrappers observe the real CJ_OS cleanup calls and their ordering.

import std.process.*
import std.time.*

@When[os == "Linux"]
foreign {
    func CJ_TEST_WrapReset(mode: Int32): Unit
    func CJ_TEST_WrapAcknowledgeWaiting(): Int32
    func CJ_TEST_WrapRetryOne(): Int32
    func CJ_TEST_WrapExitReady(): Int32
    func CJ_TEST_WrapChildReaped(): Int32
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
    println("KILL_FAILURE_FLAGS terminate=${terminateFailure} poll0=${pollZeroCalls}")
    unsafe { CJ_TEST_WrapCleanup() }
    if (terminateFailure != 1 || pollZeroCalls != 1) {
        return 11
    }
    return 0
}

@When[os == "Linux"]
func runReapNegativeControl(): Int64 {
    unsafe { CJ_TEST_WrapReset(2) }
    try {
        execute("/bin/sh", ["-c", "sleep 1"],
            stdOut: Discard, stdErr: Discard, timeout: Duration.millisecond * 10)
        unsafe { CJ_TEST_WrapCleanup() }
        return 12
    } catch (_: ProcessException) {
    }

    let pollZeroCalls = unsafe { CJ_TEST_WrapPollZeroCalls() }
    let reapNegative = unsafe { CJ_TEST_WrapReapNegativeSeen() }
    println("INJECTION_FLAGS poll0=${pollZeroCalls} reapNeg=${reapNegative}")
    if (pollZeroCalls < 50 || reapNegative != 1) {
        unsafe { CJ_TEST_WrapCleanup() }
        return 13
    }
    return 0
}

@When[os != "Linux"]
func runKillFailureControl(): Int64 { return 0 }

@When[os != "Linux"]
func runReapNegativeControl(): Int64 { return 0 }

// TASK17:16 runs only these two additions; 906bb error arms are reused.
@When[os == "Linux"]
func runRetryOneControl(): Int64 {
    unsafe { CJ_TEST_WrapReset(3) }
    var timedOut = false
    try {
        execute("/bin/sleep", ["1"], stdOut: Discard, stdErr: Discard,
            timeout: Duration.millisecond * 10)
    } catch (_: TimeoutException) {
        timedOut = true
    } catch (_: ProcessException) {
        // A retry=1 must converge, preserving the original TimeoutException.
    }
    let failedKill = unsafe { CJ_TEST_WrapTerminateFailureSeen() }
    let exitReady = unsafe { CJ_TEST_WrapExitReady() }
    let retryOne = unsafe { CJ_TEST_WrapRetryOne() }
    let reaped = unsafe { CJ_TEST_WrapChildReaped() }
    unsafe { CJ_TEST_WrapCleanup() }
    println("RETRY_ONE timedOut=${timedOut} killFailure=${failedKill} exitReady=${exitReady} retry1=${retryOne} reaped=${reaped}")
    if (!timedOut || failedKill != 1 || exitReady != 1 || retryOne != 1 || reaped != 1) {
        return 21
    }
    return 0
}

@When[os == "Linux"]
func runCleanupHandshakeControl(): Int64 {
    unsafe { CJ_TEST_WrapReset(4) }
    let observer: Future<Int64> = spawn {=>
        let start = MonoTime.now()
        while (MonoTime.now() - start < Duration.second) {
            if (unsafe { CJ_TEST_WrapAcknowledgeWaiting() } == 1) {
                return 1
            }
            sleep(Duration.millisecond)
        }
        return 0
    }
    var timedOut = false
    try {
        execute("/bin/sleep", ["1"], stdOut: Discard, stdErr: Discard,
            timeout: Duration.millisecond * 10)
    } catch (_: TimeoutException) {
        timedOut = true
    } catch (_: ProcessException) {
    }
    let acknowledged = observer.get()
    let beforeReap = unsafe { CJ_TEST_WrapProgressBeforeReap() }
    let pollZeroCalls = unsafe { CJ_TEST_WrapPollZeroCalls() }
    let reaped = unsafe { CJ_TEST_WrapChildReaped() }
    unsafe { CJ_TEST_WrapCleanup() }
    println("CLEANUP_HANDSHAKE timedOut=${timedOut} ack=${acknowledged} beforeRealReap=${beforeReap} poll0=${pollZeroCalls} reaped=${reaped}")
    if (!timedOut || acknowledged != 1 || beforeReap != 1 || pollZeroCalls < 1 || reaped != 1) {
        return 22
    }
    return 0
}

@When[os != "Linux"]
func runRetryOneControl(): Int64 { return 77 }
@When[os != "Linux"]
func runCleanupHandshakeControl(): Int64 { return 77 }

main(args: Array<String>): Int64 {
    if (args.size == 2 && args[1] == "followup") {
        let retryResult = runRetryOneControl()
        if (retryResult != 0) { return retryResult }
        return runCleanupHandshakeControl()
    }
    let killFailureResult = runKillFailureControl()
    if (killFailureResult != 0) {
        return killFailureResult
    }
    runReapNegativeControl()
}
