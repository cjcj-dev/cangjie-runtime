// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.

import std.io.*
import std.process.*
import std.runtime.*

// A thread blocked reading one child pipe must not hold array raw data. A GC
// pause requested meanwhile blocks JNI critical first (ZGC zGeneration.cpp
// VM_ZOperation::pause, zJNICritical.cpp block) and waits for every holder, so
// the reader of the other pipe would wait on the pause while the child waits
// for that pipe to drain before it writes the first pipe.
const OUT_BYTES: Int64 = 2097152

func drain(stream: InputStream): Int64 {
    let buf = Array<Byte>(4096, repeat: 0)
    var total: Int64 = 0
    while (true) {
        let n = stream.read(buf)
        if (n <= 0) {
            break
        }
        total += n
    }
    total
}

main(): Int64 {
    let child = launch("/bin/sh", ["-c", "head -c ${OUT_BYTES} /dev/zero; echo done >&2"],
        stdOut: Pipe, stdErr: Pipe)
    let errStream = child.stdErrPipe
    let outStream = child.stdOutPipe
    // Park one reader inside the stderr read before the collector starts.
    let errReader = spawn { drain(errStream) }
    sleep(Duration.millisecond * 300)
    let collector = spawn { gc(heavy: true) }
    sleep(Duration.millisecond * 300)
    let outTotal = drain(outStream)
    collector.get()
    let errTotal = errReader.get()
    if (child.wait() != 0) {
        return 1
    }
    if (outTotal != OUT_BYTES) {
        return 2
    }
    if (errTotal != 5) {
        return 3
    }
    return 0
}
