/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.process

import std.collection.*
import std.fs.*
import std.io.*
import std.sync.*
import std.time.MonoTime

/**
 * SubProcess
 *
 */
@When[os != "iOS"]
public class SubProcess <: Process {
    var _stdIn: OutputStream
    // Non-iOS exposes streams from fields initialized by the constructor.
    var _stdOut: InputStream
    var _stdErr: InputStream

    // Once the child is reaped, retain its status so wait() is idempotent. The
    // mutex also serializes the native poll and cache update: concurrent waits
    // cannot race one another into ECHILD after one caller reaps the child.
    var _waitReaped: Bool = false
    var _waitExitCode: Int64 = -1
    var _waitMutex: Mutex = Mutex()
    var _stdStreamMutex: Mutex = Mutex()

    init(
        pid: Int64,
        name: ?String,
        command: ?String,
        arguments: ?Array<String>,
        commandLine: ?Array<String>,
        // Non-iOS stores directory and environment metadata directly on Process.
        workingDirectory: ?Path,
        environment: ?Map<String, String>,
        stdIn: OutputStream,
        stdOut: InputStream,
        stdErr: InputStream
    ) {
        super(pid, name, command, arguments, commandLine, workingDirectory, environment)
        // Non-iOS stores stream objects directly on the process wrapper.
        this._stdIn = stdIn
        this._stdOut = stdOut
        this._stdErr = stdErr
    }

    @When[os == "Windows"]
    ~init() {
        closeProcessHandleIfNeeded(this._handle)
    }

    public prop stdInPipe: OutputStream {
        get() {
            this._stdIn
        }
    }
    public prop stdOutPipe: InputStream {
        get() {
            this._stdOut
        }
    }
    public prop stdErrPipe: InputStream {
        get() {
            this._stdErr
        }
    }

    @Deprecated[message: "Use `public prop stdErrPipe: InputStream` instead."]
    public prop stdErr: InputStream {
        get() {
            this._stdErr
        }
    }

    @Deprecated[message: "Use `public prop stdOutPipe: InputStream` instead."]
    public prop stdOut: InputStream {
        get() {
            this._stdOut
        }
    }

    @Deprecated[message: "Use `public prop stdInPipe: OutputStream` instead."]
    public prop stdIn: OutputStream {
        get() {
            this._stdIn
        }
    }

    public func wait(timeout!: ?Duration = None): Int64 {
        subProcessWait(this, timeout)
    }

    public func waitOutput(): (Int64, Array<Byte>, Array<Byte>) {
        subProcessWaitOutput(this)
    }

    func closeStdStream(): Unit {
        subProcessCloseStdStream(this)
    }
}

@When[os == "iOS"]
@Deprecated
public class SubProcess <: Process {
    var _stdIn: OutputStream
    // iOS keeps pipe storage behind native accessors.
    var _stdOut: InputStream
    var _stdErr: InputStream

    // See the non-iOS SubProcess for the wait-state cache contract.
    var _waitReaped: Bool = false
    var _waitExitCode: Int64 = -1
    var _waitMutex: Mutex = Mutex()
    var _stdStreamMutex: Mutex = Mutex()

    init(
        pid: Int64,
        name: ?String,
        command: ?String,
        arguments: ?Array<String>,
        commandLine: ?Array<String>,
        workingDirectory: ?Path,
        environment: ?Map<String, String>,
        stdIn: OutputStream,
        stdOut: InputStream,
        stdErr: InputStream
    ) {
        super(pid, name, command, arguments, commandLine, workingDirectory, environment)
        this._stdIn = stdIn
        this._stdOut = stdOut
        this._stdErr = stdErr
    }

    @When[os == "Windows"]
    ~init() {
        closeProcessHandleIfNeeded(this._handle)
    }

    public prop stdInPipe: OutputStream {
        get() {
            subProcessStdInPipe(this)
        }
    }
    public prop stdOutPipe: InputStream {
        get() {
            subProcessStdOutPipe(this)
        }
    }
    public prop stdErrPipe: InputStream {
        get() {
            subProcessStdErrPipe(this)
        }
    }

    @Deprecated[message: "Use `public prop stdErrPipe: InputStream` instead."]
    public prop stdErr: InputStream {
        get() {
            subProcessStdErrPipe(this)
        }
    }

    @Deprecated[message: "Use `public prop stdOutPipe: InputStream` instead."]
    public prop stdOut: InputStream {
        get() {
            subProcessStdOutPipe(this)
        }
    }

    @Deprecated[message: "Use `public prop stdInPipe: OutputStream` instead."]
    public prop stdIn: OutputStream {
        get() {
            subProcessStdInPipe(this)
        }
    }

    public func wait(timeout!: ?Duration = None): Int64 {
        return subProcessWait(this, timeout)
    }

    public func waitOutput(): (Int64, Array<Byte>, Array<Byte>) {
        return subProcessWaitOutput(this)
    }

    func closeStdStream(): Unit {
        subProcessCloseStdStream(this)
    }
}

private func setSubProcessStreams(
    process: SubProcess,
    stdIn: OutputStream,
    stdOut: InputStream,
    stdErr: InputStream
): Unit {
    process._stdIn = stdIn
    process._stdOut = stdOut
    process._stdErr = stdErr
}

private func subProcessStdInPipe(process: SubProcess): OutputStream {
    process._stdIn
}

private func subProcessStdOutPipe(process: SubProcess): InputStream {
    process._stdOut
}

private func subProcessStdErrPipe(process: SubProcess): InputStream {
    process._stdErr
}

private func subProcessWait(process: SubProcess, timeout: ?Duration): Int64 {
    let exitCode: Int64 = getWaitExitCode(process, timeout ?? Duration.Zero)
    process.closeStdStream()
    return exitCode
}

// Best-effort reclamation for a SubProcess whose timed wait just expired while
// the caller only holds a local reference (Process.execute / Process.run). Once
// the TimeoutException propagates, the instance is unreachable and nobody will
// ever call waitpid() on the child again, so on Unix it would stay a zombie
// forever. Spawn a detached reaper that blocks until the child exits and then
// discards the status. Because the instance is unreachable, this cannot steal
// the exit status from a later wait() on the same object.
@When[os == "Windows"]
func reapSubProcessAfterTimeout(process: SubProcess): Unit {
    // Windows process handles do not create zombies, but execute() still must
    // stop a timed-out child before the local SubProcess becomes unreachable.
    process.terminate(force: true)
}

@When[os != "Windows"]
func reapSubProcessAfterTimeout(process: SubProcess): Unit {
    if (process._waitReaped) {
        return
    }
    // Probe the natural-exit race before attempting to kill the child.
    let (probeResult, _) = pollSubProcessExit(process)
    if (probeResult == 1) {
        return
    }
    if (probeResult < 0) {
        throw ProcessException("Failed to reap process '${process.pid}'.")
    }

    // A live child must be terminated before the local SubProcess becomes
    // unreachable. Do not hide a failed kill behind an unbounded waiter.
    try {
        process.terminate(force: true)
    } catch (e: ProcessException) {
        let (retryResult, _) = pollSubProcessExit(process)
        if (retryResult == 1) {
            return
        }
        throw e
    }

    // Kill succeeded. Reap through the existing non-blocking native boundary
    // so the managed scheduler can make progress between probes; a blocking
    // waitpid here would pin the carrier until the child exits.
    while (true) {
        let (reapResult, _) = pollSubProcessExit(process)
        if (reapResult == 1) {
            return
        }
        if (reapResult < 0) {
            throw ProcessException("Failed to reap process '${process.pid}'.")
        }
        sleep(Duration.millisecond)
    }
}

private func subProcessWaitOutput(process: SubProcess): (Int64, Array<Byte>, Array<Byte>) {
    let outStreamFuture: Future<Array<Byte>> = spawn {
        => try {
            let outByteArrStream: ByteBuffer = readToByteBuffer(process._stdOut)
            return readToEnd(outByteArrStream)
        } catch (_: ProcessException) {
            throw ProcessException("OutputStream is invalid.")
        }
    }

    let errStreamFuture: Future<Array<Byte>> = spawn {
        => try {
            let errByteArrStream: ByteBuffer = readToByteBuffer(process._stdErr)
            return readToEnd(errByteArrStream)
        } catch (_: ProcessException) {
            throw ProcessException("ErrorStream is invalid.")
        }
    }

    let outArr: Array<Byte> = outStreamFuture.get()
    let errArr: Array<Byte> = errStreamFuture.get()
    let exitCode: Int64 = getWaitExitCode(process, Duration.Zero)

    process.closeStdStream()
    return (exitCode, outArr, errArr)
}

private func subProcessCloseStdStream(process: SubProcess): Unit {
    synchronized(process._stdStreamMutex) {
        if (let Some(stdInStream) <- (process._stdIn as ProcessOutputStream)) {
            if (let Some(stdInPipeOutputStream) <- (stdInStream.outputStream as PipeOutputStream)) {
                try {
                    stdInPipeOutputStream.close()
                } finally {
                    stdInStream.outputStream = NullProcessStream()
                }
            }
        }

        if (let Some(stdOutStream) <- (process._stdOut as ProcessInputStream)) {
            if (let Some(stdOutPipeInputStream) <- (stdOutStream.inputStream as PipeInputStream)) {
                try {
                    stdOutPipeInputStream.close()
                } finally {
                    stdOutStream.inputStream = NullProcessStream()
                }
            }
        }

        if (let Some(stdErrStream) <- (process._stdErr as ProcessInputStream)) {
            if (let Some(stdErrPipeInputStream) <- (stdErrStream.inputStream as PipeInputStream)) {
                try {
                    stdErrPipeInputStream.close()
                } finally {
                    stdErrStream.inputStream = NullProcessStream()
                }
            }
        }
    }
}

@When[os == "Windows"]
func getWaitExitCode(process: SubProcess, timeout: Duration): Int64 {
    if (let Some(exitCode) <- cachedWaitExitCode(process)) {
        return exitCode
    }
    let pid: Int32 = Int32(process.pid)
    var exitCode: Int64 = -1
    if (timeout > Duration.Zero) {
        exitCode = waitSubProcessExitTimeout(process, pid, timeout)
    } else {
        var waitExitInfo_cp: CPointer<ExitInfo> = CPointer<ExitInfo>()
        try {
            waitExitInfo_cp = unsafe { CJ_OS_WaitSubProcessExit(process._handle) }
            exitCode = parseExitInfoPointer(waitExitInfo_cp, pid)
        } finally {
            if (!waitExitInfo_cp.isNull()) {
                unsafe { LibC.free(waitExitInfo_cp) }
            }
        }
    }
    rememberWaitExitCode(process, exitCode)
}

@When[os == "Windows"]
func waitSubProcessExitTimeout(process: SubProcess, pid: Int32, timeout: Duration): Int64 {
    // A timed-out wait leaves the process handle open for a later wait().
    let waitDuration: Duration = if (timeout >= MAX_TIMEOUT_DURATION) {
        MAX_TIMEOUT_DURATION
    } else {
        timeout
    }
    let start: MonoTime = MonoTime.now()
    var exitCode: Int64 = -1
    while (true) {
        let remaining: Duration = waitDuration - (MonoTime.now() - start)
        // Probe once at the deadline to observe an already exited child.
        let timeoutMs: UInt32 = if (remaining <= Duration.Zero) {
            0
        } else {
            timeoutToMilliseconds(remaining)
        }
        let ret: Int32 = unsafe { CJ_OS_WaitSubProcessExitTimeout(process._handle, timeoutMs, inout exitCode) }
        if (ret == 1) {
            return exitCode
        }
        if (ret == -1) {
            throw ProcessException("Process '${pid}' does not exist.")
        }
        if (remaining <= Duration.Zero) {
            throw TimeoutException()
        }
        // WaitForSingleObject accepts whole milliseconds; sleep the remainder.
        if (timeoutMs == 0) {
            let fraction: Duration = waitDuration - (MonoTime.now() - start)
            if (fraction > Duration.Zero) {
                sleep(fraction)
            }
        }
    }
    return -1
}

@When[os == "Windows"]
func timeoutToMilliseconds(timeout: Duration): UInt32 {
    // Keep the fractional remainder for a separate short sleep. Reserve
    // UInt32.Max for INFINITE; a long finite wait is handled in chunks above.
    let millis: Int64 = timeout.toMilliseconds()
    if (millis >= Int64(UInt32.Max) - 1) {
        return UInt32.Max - 1
    }
    return UInt32(millis)
}

@When[os == "Windows"]
func parseExitInfoPointer(waitExitInfo_cp: CPointer<ExitInfo>, pid: Int32): Int64 {
    if (waitExitInfo_cp.isNull()) {
        throw ProcessException("Process '${pid}' does not exist.")
    }
    let waitExitInfo: ExitInfo = unsafe { waitExitInfo_cp.read() }
    if (waitExitInfo.error) {
        throw ProcessException("Process '${pid}' does not exist.")
    }
    return waitExitInfo.exitCode
}

@When[os != "Windows"]
func getWaitExitCode(process: SubProcess, timeout: Duration): Int64 {
    // Poll with waitpid(WNOHANG) for both bounded and unbounded waits. This
    // keeps each native operation short, allowing concurrent callers to make
    // progress while another caller waits, and leaves no background waiter on
    // timeout. A zero timeout means no deadline (the public None form).
    let waitDuration: Duration = if (timeout >= MAX_TIMEOUT_DURATION) { // cjlint-ignore !G.VAR.02
        MAX_TIMEOUT_DURATION
    } else {
        timeout
    }
    let bounded: Bool = timeout > Duration.Zero
    let start: MonoTime = MonoTime.now() // cjlint-ignore !G.VAR.02
    while (true) {
        let (ret, exitCode) = pollSubProcessExit(process)
        if (ret == 1) {
            return exitCode
        }
        if (ret < 0) {
            // Another caller can only reap while holding the same mutex as the
            // poll, so an error here is a real native wait error, not a cache
            // race. Preserve the existing -1 error propagation contract.
            return -1
        }
        if (bounded) {
            let remaining: Duration = waitDuration - (MonoTime.now() - start)
            if (remaining <= Duration.Zero) {
                // Probe at the deadline has already happened in this iteration;
                // report timeout only while the child is still running.
                throw TimeoutException() // cjlint-ignore !G.ERR.01
            }
            sleep(if (remaining < Duration.millisecond) { remaining } else { Duration.millisecond })
        } else {
            sleep(Duration.millisecond)
        }
    }
    return -1
}

private func cachedWaitExitCode(process: SubProcess): ?Int64 {
    synchronized(process._waitMutex) {
        if (process._waitReaped) {
            Some(process._waitExitCode)
        } else {
            None
        }
    }
}

private func rememberWaitExitCode(process: SubProcess, exitCode: Int64): Int64 {
    synchronized(process._waitMutex) {
        if (!process._waitReaped) {
            process._waitExitCode = exitCode
            process._waitReaped = true
        }
        process._waitExitCode
    }
}

@When[os != "Windows"]
private func pollSubProcessExit(process: SubProcess): (Int32, Int64) {
    synchronized(process._waitMutex) {
        if (process._waitReaped) {
            return (1, process._waitExitCode)
        }
        var exitCode: Int64 = -1 // cjlint-ignore !G.VAR.02
        let ret: Int32 = unsafe {
            CJ_OS_WaitSubProcessExitNonBlocking(Int32(process.pid), inout exitCode)
        }
        if (ret == 1) {
            process._waitExitCode = exitCode
            process._waitReaped = true
        }
        return (ret, exitCode)
    }
}
