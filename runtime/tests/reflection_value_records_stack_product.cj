// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
// Real language consumer for the lifetime of MethodInfo::ApplyCJMethod value
// records across a CJThread stack move. Run with cjStackSize=64kb: every case
// runs on a fresh cjthread, and GROW_DEPTH frames of deep() need megabytes, so
// a grown case only completes if the stack was reallocated and copied while
// the reflective native frame and its records were live. Depth 0 is the
// no-growth control. Two 32KiB records either fit above the stack guard and
// equal the direct call, or raise the ordinary StackOverflowError. Exit code
// is the number of mismatches or crashes.
import std.reflect.*
import std.runtime.*
import std.sync.*

let GROW_DEPTH: Int64 = 20000

func deep(n: Int64): Int64 {
    if (n <= 0) {
        return 0
    }
    let pad = VArray<Int64, $8>(repeat: n)
    let r = deep(n - 1)
    return r + pad[7] - n + 1
}

public struct Rec {
    public let a: String
    public let b: Array<Int64>
    public let c: String

    public init(a: String, b: Array<Int64>, c: String) {
        this.a = a
        this.b = b
        this.c = c
    }

    public func grown(depth: Int64): String {
        let d = deep(depth)
        gc(heavy: true)
        "${a}|${b[1]}|${c}|${d}"
    }
}

public struct Built {
    public let x: String
    public let y: Array<String>
    public let z: String

    public init(x: String, y: Array<String>, depth: Int64) {
        let d = deep(depth)
        gc(heavy: true)
        this.x = x + "#${d}"
        this.y = y
        this.z = y[0] + x
    }
}

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

public class Probe {
    public static func pass(s: String, r: Rec, depth: Int64): String {
        let d = deep(depth)
        gc(heavy: true)
        "${s}|${r.a}|${r.b[2]}|${r.c}|${d}"
    }

    public static func raise(r: Rec, depth: Int64): String {
        let d = deep(depth)
        gc(heavy: true)
        throw IllegalStateException("boom:${r.a}:${r.c}:${d}")
    }

    public static func big2(x: Big, y: Big, depth: Int64): String {
        let d = deep(depth)
        gc(heavy: true)
        "${x.head}|${x.body[4095]}|${x.tail}|${y.head}|${y.body[0]}|${y.tail}|${d}"
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

func begin(name: String): Unit {
    eprintln("BEGIN ${name}")
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

main(args: Array<String>): Int64 {
    // Optional group filter: control-small, control-big, grown-small, grown-big.
    let only = if (args.size > 0) { args[0] } else { "" }
    let want = { g: String => only == "" || only == g }
    var bad = 0
    var soe = 0
    let strT = StructTypeInfo.get("std.core.String")
    let intT = TypeInfo.of<Int64>()
    let recT = StructTypeInfo.of<Rec>()
    let builtT = StructTypeInfo.of<Built>()
    let bigT = StructTypeInfo.of<Big>()
    let probeT = ClassTypeInfo.of<Probe>()
    let passF = probeT.getStaticFunction("pass", [strT, recT, intT])
    let raiseF = probeT.getStaticFunction("raise", [recT, intT])
    let big2F = probeT.getStaticFunction("big2", [bigT, bigT, intT])
    let grownF = recT.getInstanceFunction("grown", [intT])
    let builtC = builtT.getConstructor([strT, TypeInfo.of<Array<String>>(), intT])

    // Growth must really be available on these cjthreads.
    begin("direct deep")
    if (only == "" || only.startsWith("grown")) {
        bad += report("direct deep on fresh cjthread", onFresh({ => "${deep(GROW_DEPTH)}" }), "${GROW_DEPTH}")
    }

    for (depth in [0, GROW_DEPTH]) {
        let tag = if (depth == 0) { "control" } else { "grown" }
        let r = Rec(heapStr("ra", depth), [depth, depth + 1, depth + 2], heapStr("rc", depth))
        let s = heapStr("s", depth)
        if (want("${tag}-small")) {
        begin("${tag} static")
        try {
            let got = onFresh({ => (passF.apply(probeT, [s, r, depth]) as String).getOrThrow() })
            bad += report("${tag} static(String, Rec)", got, onFresh({ => Probe.pass(s, r, depth) }))
        } catch (e: Exception) { eprintln("FAIL ${tag} static(String, Rec): ${e}"); bad += 1 }
        begin("${tag} receiver")
        try {
            let got = onFresh({ => (grownF.apply(r, [depth]) as String).getOrThrow() })
            bad += report("${tag} value receiver", got, onFresh({ => r.grown(depth) }))
        } catch (e: Exception) { eprintln("FAIL ${tag} value receiver: ${e}"); bad += 1 }
        begin("${tag} struct init")
        try {
            let y = [heapStr("y", depth), heapStr("w", depth)]
            let x = heapStr("x", depth)
            let got = onFresh({ =>
                let q = (builtC.apply([x, y, depth]) as Built).getOrThrow()
                "${q.x}|${q.y[1]}|${q.z}"
            })
            let want = onFresh({ =>
                let q = Built(x, y, depth)
                "${q.x}|${q.y[1]}|${q.z}"
            })
            bad += report("${tag} struct init", got, want)
        } catch (e: Exception) { eprintln("FAIL ${tag} struct init: ${e}"); bad += 1 }
        begin("${tag} exceptional")
        try {
            let got = onFresh({ =>
                try {
                    raiseF.apply(probeT, [r, depth])
                    "no exception"
                } catch (e: IllegalStateException) {
                    e.message
                }
            })
            let want = onFresh({ =>
                try {
                    Probe.raise(r, depth)
                } catch (e: IllegalStateException) {
                    e.message
                }
            })
            bad += report("${tag} exceptional return", got, want)
        } catch (e: Exception) { eprintln("FAIL ${tag} exceptional return: ${e}"); bad += 1 }
        }
        if (want("${tag}-big")) {
        for (before in [0, 200, 400]) {
            begin("${tag} big after ${before}")
            try {
                let x = Big(heapStr("bh", before), before + 1, heapStr("bt", depth))
                let y = Big(heapStr("ch", depth), before + 2, heapStr("ct", before))
                // Records that do not fit above the stack guard raise the ordinary
                // StackOverflowError before any record is written (C2NStub check).
                let got = onFresh({ =>
                    try {
                        nested(before, { => (big2F.apply(probeT, [x, y, depth]) as String).getOrThrow() })
                    } catch (e: StackOverflowError) {
                        "StackOverflowError"
                    }
                })
                let want = onFresh({ => nested(before, { => Probe.big2(x, y, depth) }) })
                if (got == "StackOverflowError") {
                    eprintln("PASS ${tag} two 32KiB records after ${before} frames: controlled StackOverflowError")
                    soe += 1
                } else {
                    bad += report("${tag} two 32KiB records after ${before} frames", got, want)
                }
            } catch (e: Exception) { eprintln("FAIL ${tag} two 32KiB records after ${before} frames: ${e}"); bad += 1 }
        }
        }
    }
    println("STACK_MOVE_SOE=${soe}")
    eprintln("STACK_MOVE_SOE=${soe} STACK_MOVE_BAD=${bad}")
    println("STACK_MOVE_BAD=${bad}")
    return bad
}
