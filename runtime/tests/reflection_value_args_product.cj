// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
// Real language consumer for MethodInfo::ApplyCJMethod value records
// (ScopedAllocBuffer::StageValueRecord): compile with cjc --dy-std against the
// paired runtime and run; exit code 0 means every reflected call equals the
// direct call. Each callee collects before it reads its value parameters.
import std.reflect.*
import std.runtime.*

public struct ValuePair {
    public let label: String
    public let items: Array<Int64>
    public let n: Int64

    public init(label: String, items: Array<Int64>, n: Int64) {
        gc()
        let pad = Array<Int64>(512, repeat: 3)
        this.label = label
        this.items = items
        this.n = n + pad.size - 512
    }

    public func describe(suffix: String): String {
        gc(heavy: true)
        let pad = Array<String>(128, repeat: "p")
        "${label}/${items.size}/${n}/${suffix}/${pad.size}"
    }
}

public class ValueHolder {
    public var name: String = ""
    public var pair: ValuePair = ValuePair("", [], 0)

    public init() {}

    public init(name: String) {
        this.name = name
    }

    public init(name: String, pair: ValuePair) {
        gc(heavy: true)
        let junk = Array<String>(256, repeat: "x")
        this.name = name + junk[0][0..0]
        this.pair = pair
    }

    public func join(p: ValuePair, s: String): String {
        gc(heavy: true)
        "${name}|${p.label}|${p.items.size}|${p.n}|${s}"
    }

    public static func concat(a: String, p: ValuePair): String {
        gc()
        "${a}+${p.label}+${p.n}"
    }
}

func report(name: String, got: String, want: String): Int64 {
    if (got == want) {
        println("PASS ${name}: ${got}")
        return 0
    }
    println("FAIL ${name}: got=${got} want=${want}")
    return 1
}

main(): Int64 {
    var bad = 0
    let strT = StructTypeInfo.get("std.core.String")
    let pairT = StructTypeInfo.of<ValuePair>()
    let holderT = ClassTypeInfo.of<ValueHolder>()
    let label = "lab" + "el${bad}"
    let items: Array<Int64> = [1, 2, 3]
    let p = ValuePair(label, items, 42)

    try {
        let r = (holderT.getConstructor([strT]).apply(["hello"]) as ValueHolder).getOrThrow()
        bad += report("class init(String) literal", r.name, ValueHolder("hello").name)
    } catch (e: Exception) { println("FAIL class init(String) literal: ${e}"); bad += 1 }
    try {
        let src = "xx" + label + "yy"
        let sub = src[2..8]
        let r = (holderT.getConstructor([strT]).apply([sub]) as ValueHolder).getOrThrow()
        bad += report("class init(String) heap substring", r.name, ValueHolder(sub).name)
    } catch (e: Exception) { println("FAIL class init(String) heap substring: ${e}"); bad += 1 }
    try {
        var loopBad = 0
        for (i in 0..300) {
            let s = "v${i}-" + label
            let r = (holderT.getConstructor([strT]).apply([s]) as ValueHolder).getOrThrow()
            if (r.name != s) { loopBad += 1 }
        }
        bad += report("class init(String) x300", "${loopBad}", "0")
    } catch (e: Exception) { println("FAIL class init(String) x300: ${e}"); bad += 1 }
    try {
        let r = (holderT.getConstructor([strT, pairT]).apply([label, p]) as ValueHolder).getOrThrow()
        let d = ValueHolder(label, p)
        bad += report("class init(String, struct) gc in callee",
            "${r.name}|${r.pair.label}|${r.pair.items.size}|${r.pair.n}",
            "${d.name}|${d.pair.label}|${d.pair.items.size}|${d.pair.n}")
    } catch (e: Exception) { println("FAIL class init(String, struct): ${e}"); bad += 1 }
    try {
        let h = ValueHolder("hh")
        let got = (holderT.getInstanceFunction("join", [pairT, strT]).apply(h, [p, "s3"]) as String).getOrThrow()
        bad += report("instance func(struct, String)", got, h.join(p, "s3"))
    } catch (e: Exception) { println("FAIL instance func(struct, String): ${e}"); bad += 1 }
    try {
        let got = (holderT.getStaticFunction("concat", [strT, pairT]).apply(holderT, [label, p]) as String).getOrThrow()
        bad += report("static func(String, struct)", got, ValueHolder.concat(label, p))
    } catch (e: Exception) { println("FAIL static func(String, struct): ${e}"); bad += 1 }
    try {
        let got = (pairT.getInstanceFunction("describe", [strT]).apply(p, ["sfx"]) as String).getOrThrow()
        bad += report("struct receiver func(String)", got, p.describe("sfx"))
    } catch (e: Exception) { println("FAIL struct receiver func(String): ${e}"); bad += 1 }
    try {
        let ctor = pairT.getConstructor([strT, TypeInfo.of<Array<Int64>>(), TypeInfo.of<Int64>()])
        let q = (ctor.apply([label, items, 7]) as ValuePair).getOrThrow()
        let d = ValuePair(label, items, 7)
        bad += report("struct init(String, Array, Int64)", "${q.label}/${q.items.size}/${q.n}",
            "${d.label}/${d.items.size}/${d.n}")
    } catch (e: Exception) { println("FAIL struct init(String, Array, Int64): ${e}"); bad += 1 }
    println("VALUE_ARGS_BAD=${bad}")
    return bad
}
