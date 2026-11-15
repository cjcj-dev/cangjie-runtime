// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
// Direct (non-reflective) producer of the null-$BP stack-record convention: a
// stack-local struct is initialised, and later mutated, by ordinary calls whose
// callee grows the CJThread stack (GROW_DEPTH frames of deep()) and collects
// while the record is live, before storing several reference fields through
// its receiver. The caller collects again after the call and reads the record.
// Run with cjStackSize=64kb, one process per group: "control" (depth 0) or
// "grown" (GROW_DEPTH). Exit code is the number of mismatches.
import std.runtime.*

let GROW_DEPTH: Int64 = 20000

class Box {
    Box(let tag: String) {}
}

func deep(n: Int64): Int64 {
    if (n <= 0) {
        return 0
    }
    let pad = VArray<Int64, $8>(repeat: n)
    let r = deep(n - 1)
    return r + pad[7] - n + 1
}

func heapStr(p: String, i: Int64): String {
    p + "-" + "${i * 7}" + "-" + p
}

struct Built {
    var x: String
    var y: Array<String>
    var z: String
    var b: Box

    init(x: String, y: Array<String>, depth: Int64) {
        let d = deep(depth)
        gc(heavy: true)
        this.x = x + "#${d}"
        this.y = y
        this.z = y[0] + x
        this.b = Box(heapStr("b", d))
    }

    mut func regrow(tail: String, depth: Int64): Int64 {
        let d = deep(depth)
        gc(heavy: true)
        this.x = this.x + tail
        this.b = Box(this.b.tag + tail)
        this.y = [this.y[1], tail]
        return d
    }
}

func render(q: Built): String {
    "${q.x}|${q.y[0]}|${q.y[1]}|${q.z}|${q.b.tag}"
}

func check(name: String, got: String, want: String): Int64 {
    if (got == want) {
        eprintln("PASS ${name}: ${got}")
        return 0
    }
    eprintln("FAIL ${name}: got=${got} want=${want}")
    return 1
}

main(args: Array<String>): Int64 {
    let depth = if (args.size > 0 && args[0] == "grown") { GROW_DEPTH } else { 0 }
    let y = [heapStr("y", depth), heapStr("w", depth)]
    let x = heapStr("x", depth)
    let t = heapStr("t", depth)
    eprintln("BEGIN direct null-base stack record depth=${depth}")
    var bad = 0
    let initGot = spawn {
        let q = Built(x, y, depth)
        gc(heavy: true)
        render(q)
    }.get()
    bad += check("direct struct init depth=${depth}", initGot,
        "${x}#${depth}|${y[0]}|${y[1]}|${y[0]}${x}|${heapStr("b", depth)}")
    let mutGot = spawn {
        var q = Built(x, y, 0)
        let d = q.regrow(t, depth)
        gc(heavy: true)
        "${d}|" + render(q)
    }.get()
    bad += check("direct mut receiver depth=${depth}", mutGot,
        "${depth}|${x}#0${t}|${y[1]}|${t}|${y[0]}${x}|${heapStr("b", 0)}${t}")
    return bad
}
