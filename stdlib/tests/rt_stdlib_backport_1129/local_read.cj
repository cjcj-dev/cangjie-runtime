// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.

import std.fs.*

private func checkLocal(condition: Bool, message: String): Unit {
    if (!condition) {
        throw IllegalStateException(message)
    }
}

private func checkExplicitLocalPaths(path: String): Unit {
    File.writeTo(path, [11, 22])
    let localPath: String @local! = String(path) @local!
    let stream: File @local! = File(localPath, OpenMode.Read) @local!
    try {
        let buffer: Array<Byte> @local! = Array<Byte>(3, repeat: 0) @local!
        checkLocal(stream.read(buffer) == 2, "explicit local read length")
        checkLocal(buffer[0] == 11 && buffer[1] == 22, "explicit local read bytes")
    } finally {
        stream.close()
    }
    println("PASS: explicit local File.read")
}

main(args: Array<String>): Int64 {
    try {
        checkExplicitLocalPaths(args[1])
    } finally {
        removeIfExists(args[1])
    }
    return 0
}
