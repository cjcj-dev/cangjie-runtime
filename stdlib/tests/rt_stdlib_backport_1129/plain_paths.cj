// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.

import std.convert.*
import std.env.getStdIn
import std.fs.*
import std.runtime.{gc, getGCCount}

private func checkPlain(condition: Bool, message: String): Unit {
    if (!condition) {
        throw IllegalStateException(message)
    }
}

private func checkPlainParse(): Unit {
    let prefixed: String = "0x7f"
    checkPlain(Int64.parse(prefixed) == 127, "plain prefixed integer")
    checkPlain(Int64.parse("-9223372036854775808") == Int64.Min, "plain Int64 minimum")
    checkPlain(UInt64.parse("18446744073709551615") == UInt64.Max, "plain UInt64 maximum")
    checkPlain(Int64.tryParse("9223372036854775808").isNone(), "plain overflow")
    checkPlain(Int64.tryParse("0xg").isNone(), "plain invalid prefixed digit")
    println("PASS: plain integer boundaries, radix prefix and invalid input")
}

private func checkPlainRead(path: String): Unit {
    File.writeTo(path, [11, 22, 33, 44, 55])
    let stream: File = File(path, OpenMode.Read)
    try {
        let buffer: Array<Byte> = Array<Byte>(3, repeat: 0)
        checkPlain(stream.read(buffer) == 3, "plain first read length")
        checkPlain(buffer[0] == 11 && buffer[1] == 22 && buffer[2] == 33, "plain first read bytes")
        checkPlain(stream.read(buffer) == 2, "plain short read length")
        checkPlain(buffer[0] == 44 && buffer[1] == 55 && buffer[2] == 33, "plain short read preserves suffix")
        checkPlain(stream.read(buffer) == 0, "plain EOF")
        checkPlain(buffer[0] == 44 && buffer[1] == 55 && buffer[2] == 33, "plain EOF preserves buffer")
        var emptyRejected = false
        try {
            stream.read(Array<Byte>())
        } catch (_: IllegalArgumentException) {
            emptyRejected = true
        }
        checkPlain(emptyRejected, "plain empty read is rejected")
    } finally {
        stream.close()
    }
    var closedRejected = false
    try {
        stream.read(Array<Byte>(1, repeat: 0))
    } catch (_: FSException) {
        closedRejected = true
    }
    checkPlain(closedRejected, "plain closed read is rejected")
    println("PASS: plain File.read bytes, short read, EOF and error paths")
}

// The driver waits for File.read to block in the FIFO's native read before
// allowing the collection. It supplies the byte only after that collection.
private func checkReadAcrossCollection(path: String): Unit {
    let stream: File = File(path, OpenMode.ReadWrite)
    try {
        let buffer: Array<Byte> = Array<Byte>(1, repeat: 0)
        let reader = spawn { stream.read(buffer) }
        checkPlain(getStdIn().readln() == Some("collect"), "missing native-read gate")
        let before = getGCCount()
        gc(heavy: true)
        // The collector publishes its count after waking the synchronous caller.
        var after = getGCCount()
        for (_ in 0..1000) {
            if (after > before) {
                break
            }
            sleep(Duration.millisecond)
            after = getGCCount()
        }
        checkPlain(after > before, "collection did not complete while read was waiting")
        print("COLLECTION: ${before} -> ${after}\n", flush: true)
        checkPlain(reader.get(Duration.second * 3) == 1, "FIFO read length")
        checkPlain(buffer[0] == 42, "FIFO read byte after collection")
    } finally {
        stream.close()
    }
    println("PASS: collection completed while File.read awaited native input")
}

main(args: Array<String>): Int64 {
    match (args[0]) {
        case "convert" => checkPlainParse()
        case "fs" =>
            try {
                checkPlainRead(args[1])
            } finally {
                removeIfExists(args[1])
            }
        case "blocked" => checkReadAcrossCollection(args[1])
        case _ => return 2
    }
    return 0
}
