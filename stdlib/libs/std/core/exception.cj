/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.core

@Intrinsic
func fillInStackTrace(seed: TraceCapture, message: RawArray<UInt8>): TraceCapture

@Intrinsic
func decodeStackTrace(capture: TraceCapture, frameIndex: Int64): StackTraceData

// Decoder output layout is unchanged; raw native fields remain distinct.
protected struct StackTraceData {
    public let className: RawArray<UInt8> = EMPTY_UINT8_RAW_ARRAY
    public let methodName: RawArray<UInt8> = EMPTY_UINT8_RAW_ARRAY
    public let fileName: RawArray<UInt8> = EMPTY_UINT8_RAW_ARRAY
    public let lineNumber: Int64 = 0
}

protected struct TraceFrame {
    public let mangledName: RawArray<UInt8> = EMPTY_UINT8_RAW_ARRAY
    public let directory: RawArray<UInt8> = EMPTY_UINT8_RAW_ARRAY
    public let filename: RawArray<UInt8> = EMPTY_UINT8_RAW_ARRAY
    public let resolved: StackTraceData = StackTraceData()
    public let lineNumber: Int64 = 0
    public let format: Int64 = 0
    public let kind: Int64 = 0 // native=0; interpreted=1
}

protected class TraceCapture {
    public let frames: RawArray<TraceFrame> = RawArray<TraceFrame>(0, repeat: TraceFrame())
    public let fold: Int64 = 0
    public init() {}
    public prop size: Int64 { get() { arraySize(frames) } }
    public func isEmpty(): Bool { size == 0 }
    public func frame(index: Int64): TraceFrame { arrayGet(frames, index) }
}

func getStackTraceImpl(capture: TraceCapture): Array<StackTraceElement> {
    let frameSize = capture.size
    let emptyElement = StackTraceElement("", "", "", 0)
    let stackTraceArray = Array<StackTraceElement>(frameSize, repeat: emptyElement)
    var eleSize: Int64 = 0
    for (i in 0..frameSize) {
        let data = decodeStackTrace(capture, i)
        // Ignore invalid stack trace data
        if (data.lineNumber == 0 && arraySize(data.fileName) == 0) {
            continue
        }
        if (data.lineNumber == -1) {
            continue
        }
        let stkEle = StackTraceElement(
            String(data.className),
            String(data.methodName),
            String(data.fileName),
            data.lineNumber
        )
        stackTraceArray[eleSize] = stkEle
        eleSize++
    }
    return stackTraceArray.clone(0..eleSize)
}

public open class Exception <: ToString {
    private var detailMessage: String = String.empty
    private var capture: TraceCapture = TraceCapture()
    private var traceElementArray: Array<StackTraceElement> = []
    private var innerCause: ?Exception = None

    public init() {
        capture = fillInStackTrace(TraceCapture(), detailMessage.toArray().rawptr)
    }

    public init(message: String) {
        detailMessage = message
        capture = fillInStackTrace(TraceCapture(), detailMessage.toArray().rawptr)
    }

    @When[env != "ohos"]
    public init(message: String, causedBy: Exception) {
        detailMessage = message
        innerCause = causedBy
        capture = fillInStackTrace(TraceCapture(), detailMessage.toArray().rawptr)
    }

    @When[env != "ohos"]
    public init(causedBy: Exception) {
        innerCause = causedBy
        capture = fillInStackTrace(TraceCapture(), detailMessage.toArray().rawptr)
    }

    protected open func getClassName(): String {
        return "Exception"
    }

    public open prop message: String {
        get() {
            return detailMessage
        }
    }

    /*
     * Cause of this exception, which may be null. The cause is the exception that caused this exception to get thrown.
     * It is possible that `printStackTrace` will recurse infinitely and eventually cause a stack overflow. Be careful.
     * To fix it, Exception should implement `Hashable & Equatable` and check if a cause has already been printed.
     */
    @When[env != "ohos"]
    public mut prop causedBy: ?Exception {
        get() {
            return innerCause
        }
        set(cause) {
            innerCause = cause
        }
    }

    public open func toString(): String {
        let className: String = getClassName()
        let message: String = this.message
        if (message.isEmpty()) {
            return className
        }
        return "${className}: ${message}"
    }

    public func printStackTrace(): Unit {
        printStackTrace("An exception has occurred:\n", Array<StackTraceElement>())
    }

    /*
     * Print the stack trace of this exception with a caption and enclosing trace. 
     *
     * @param caption the caption to print before the stack trace
     * @param enclosingTrace the enclosing stack trace if present (that is the stack trace of `causedBy`)
     */
    func printStackTrace(caption: String, enclosingTrace: Array<StackTraceElement>): Unit {
        eprintln("${caption}${toString()}")
        let trace = getStackTrace()
        var ourSize = trace.size - 1
        var enclosingSize = enclosingTrace.size - 1
        while (ourSize >= 0 && enclosingSize >= 0 && trace[ourSize].equals(enclosingTrace[enclosingSize])) {
            ourSize--
            enclosingSize--
        }
        let commonSize = trace.size - 1 - ourSize
        for (i in 0..=ourSize) {
            let element = trace[i]
            if (element.methodName.contains("Exception::init")) {
                continue
            }
            if (element.declaringClass.isEmpty() && element.methodName.isEmpty()) { // simple stack trace format
                eprintln("\t at ${element.fileName}:${element.lineNumber}")
            } else {
                let packageDelimiter: String = if (element.declaringClass.isEmpty()) { "" } else { "::" }
                eprintln(
                    "\t at ${element.declaringClass}${packageDelimiter}${element.methodName}(${element.fileName}:${element.lineNumber})")
            }
        }
        if (commonSize > 0) {
            eprintln("\t ... ${commonSize} more")
        }
        if (let Some(cause) <- innerCause) {
            cause.printStackTrace("Caused by: ", trace)
        }
    }

    public func getStackTrace(): Array<StackTraceElement> {
        getStackTraceImpl(capture)
    }
}

