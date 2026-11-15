// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
// Real language consumer for value records that do not fit the CJThread stack
// left at a CJ_MCC_ApplyCJ* entry. Run with cjStackSize=64kb: every case runs
// on a fresh cjthread and passes two 32KiB records after `before` consumed
// frames, so the entry has to grow the stack before ApplyCJMethod can place
// them. Each reflective call must equal the direct call at the same depth: a
// StackOverflowError here is a failure, not an accepted outcome. Callees and
// callers collect after the move, a class receiver is written by the callee
// and read back, a value receiver is read, and a throwing callee must deliver
// its own exception. Exit code is the number of mismatches or crashes.
import std.reflect.*
import std.runtime.*
import std.sync.*

public struct Big {
    public let head: String
    public let body: VArray<Int64, $4096>
    public let tail: String

    public init(head: String, fill: Int64, tail: String) {
        this.head = head
        this.body = VArray<Int64, $4096>(repeat: fill)
        this.tail = tail
    }
}

public struct Pair {
    public let a: String
    public let b: String

    public init(a: String, b: String) {
        this.a = a
        this.b = b
    }

    public func both(x: Big, y: Big): String {
        gc(heavy: true)
        "${a}|${x.head}|${x.body[4095]}|${y.tail}|${y.body[1]}|${b}"
    }
}

public class Box {
    public var s: String = ""
    public var n: Int64 = 0

    public func put(x: Big, y: Big): String {
        gc(heavy: true)
        this.s = x.head + "+" + y.tail
        this.n = x.body[0] + y.body[4095]
        "${s}|${n}"
    }
}

public class Probe {
    public static func big2(x: Big, y: Big): String {
        gc(heavy: true)
        "${x.head}|${x.body[4095]}|${x.tail}|${y.head}|${y.body[0]}|${y.tail}"
    }

    public static func raise2(x: Big, y: Big): String {
        gc(heavy: true)
        throw IllegalStateException("boom:${x.head}:${y.tail}:${x.body[1]}")
    }
}

func report(name: String, got: String, want: String): Int64 {
    if (got == want) {
        eprintln("PASS ${name}: ${got}")
        return 0
    }
    eprintln("FAIL ${name}: got=${got} want=${want}")
    return 1
}

func onFresh(body: () -> String): String {
    spawn { body() }.get()
}

func heapStr(p: String, i: Int64): String {
    p + "-" + "${i * 7}" + "-" + p
}

// Consume `before` frames first, so the reflective call starts with little of
// the current stack left.
func nested(before: Int64, body: () -> String): String {
    if (before <= 0) {
        return body()
    }
    let pad = VArray<Int64, $8>(repeat: before)
    let r = nested(before - 1, body)
    if (pad[0] < 0) {
        return ""
    }
    return r
}

// The caller's own records and references must survive the move and a collection.
func after(x: Big, y: Big, got: String): String {
    gc(heavy: true)
    "${got}#${x.head}|${x.body[7]}|${y.tail}|${y.body[4000]}"
}

func reflective(name: String, body: () -> String): String {
    try {
        body()
    } catch (e: StackOverflowError) {
        eprintln("SOE ${name}")
        "StackOverflowError"
    }
}

main(args: Array<String>): Int64 {
    // Optional group filter: static, value-receiver, class-receiver, exception.
    let only = if (args.size > 0) { args[0] } else { "" }
    let want = { g: String => only == "" || only == g }
    var bad = 0
    let bigT = StructTypeInfo.of<Big>()
    let pairT = StructTypeInfo.of<Pair>()
    let boxT = ClassTypeInfo.of<Box>()
    let probeT = ClassTypeInfo.of<Probe>()
    let big2F = probeT.getStaticFunction("big2", [bigT, bigT])
    let raise2F = probeT.getStaticFunction("raise2", [bigT, bigT])
    let bothF = pairT.getInstanceFunction("both", [bigT, bigT])
    let putF = boxT.getInstanceFunction("put", [bigT, bigT])

    for (before in [0, 400, 2000]) {
        let x = Big(heapStr("bh", before), before + 1, heapStr("bt", before))
        let y = Big(heapStr("ch", before), before + 2, heapStr("ct", before))
        if (want("static")) {
            let name = "static two 32KiB records after ${before} frames"
            try {
                let got = onFresh({ => nested(before, { => after(x, y,
                    reflective(name, { => (big2F.apply(probeT, [x, y]) as String).getOrThrow() })) }) })
                let exp = onFresh({ => nested(before, { => after(x, y, Probe.big2(x, y)) }) })
                bad += report(name, got, exp)
            } catch (e: Exception) { eprintln("FAIL ${name}: ${e}"); bad += 1 }
        }
        if (want("value-receiver")) {
            let name = "value receiver with two 32KiB records after ${before} frames"
            try {
                let p = Pair(heapStr("pa", before), heapStr("pb", before))
                let got = onFresh({ => nested(before, { => after(x, y,
                    reflective(name, { => (bothF.apply(p, [x, y]) as String).getOrThrow() })) }) })
                let exp = onFresh({ => nested(before, { => after(x, y, p.both(x, y)) }) })
                bad += report(name, got, exp)
            } catch (e: Exception) { eprintln("FAIL ${name}: ${e}"); bad += 1 }
        }
        if (want("class-receiver")) {
            let name = "class receiver written with two 32KiB records after ${before} frames"
            try {
                let got = onFresh({ => nested(before, { =>
                    let box = Box()
                    let r = reflective(name, { => (putF.apply(box, [x, y]) as String).getOrThrow() })
                    gc(heavy: true)
                    after(x, y, "${r}@${box.s}|${box.n}")
                }) })
                let exp = onFresh({ => nested(before, { =>
                    let box = Box()
                    let r = box.put(x, y)
                    gc(heavy: true)
                    after(x, y, "${r}@${box.s}|${box.n}")
                }) })
                bad += report(name, got, exp)
            } catch (e: Exception) { eprintln("FAIL ${name}: ${e}"); bad += 1 }
        }
        if (want("exception")) {
            let name = "exceptional return with two 32KiB records after ${before} frames"
            try {
                let got = onFresh({ => nested(before, { =>
                    let r = reflective(name, { =>
                        try {
                            raise2F.apply(probeT, [x, y])
                            "returned"
                        } catch (e: IllegalStateException) {
                            "caught:${e.message}"
                        }
                    })
                    after(x, y, r)
                }) })
                let exp = onFresh({ => nested(before, { =>
                    let r = try {
                        Probe.raise2(x, y)
                        "returned"
                    } catch (e: IllegalStateException) {
                        "caught:${e.message}"
                    }
                    after(x, y, r)
                }) })
                bad += report(name, got, exp)
            } catch (e: Exception) { eprintln("FAIL ${name}: ${e}"); bad += 1 }
        }
    }
    println("ENTRY_GROW_BAD=${bad}")
    eprintln("ENTRY_GROW_BAD=${bad}")
    return bad
}
