// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
import std.net.*
import std.regex.{Regex, RegexOption}

func udp(): Int64 {
    let bindAt = IPSocketAddress(IPv4Address.localhost, 0)
    let receiver = UdpSocket(bindAt: bindAt)
    let sender = UdpSocket(bindAt: bindAt)
    try {
        receiver.bind()
        sender.bind()
        receiver.receiveTimeout = Some(3 * Duration.second)
        sender.connect(receiver.localAddress)
        let bytes: Array<Byte> = [23, 89]
        sender.send(bytes)
        let buf = Array<Byte>(8, repeat: 0)
        let (_, length) = receiver.receiveFrom(buf)
        if (length != 2 || buf[0] != 23 || buf[1] != 89) {
            return 42
        }
        println("UDP_CONNECT_HANDLE_ASSERTION_PASS")
        return 0
    } finally {
        sender.close()
        receiver.close()
    }
}

func regex(): Int64 {
    let pattern = Regex("^(Cat|Dog)[0-9]+$", RegexOption().ignoreCase())
    if (!pattern.matches("cat231") || pattern.matches("cat")) {
        return 43
    }
    println("REGEX_ASSERTIONS_PASS")
    return 0
}

func spawnForTrace(): Future<Array<StackTraceElement>> {
    spawn { Exception("child marker").getStackTrace() }
}

func fromSpawn(): Int64 {
    let control = Exception("main marker").getStackTrace()
    for (frame in control) {
        if (frame.methodName.contains("spawnForTrace")) {
            return 44
        }
    }
    let trace = spawnForTrace().get()
    if (trace.size == 0) {
        return 45
    }
    println("CREATOR_LAST=${trace[trace.size - 1].methodName}")
    if (!trace[trace.size - 1].methodName.contains("spawnForTrace")) {
        return 46
    }
    println("CREATOR_ASSERTIONS_PASS")
    return 0
}

main(args: Array<String>): Int64 {
    if (args.size != 1) {
        return 47
    }
    match (args[0]) {
        case "udp" => udp()
        case "regex" => regex()
        case "creator" => fromSpawn()
        case _ => 48
    }
}
