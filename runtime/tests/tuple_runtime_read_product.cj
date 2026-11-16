// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
// Exercise the installed compiler/runtime tuple predicate with live references.
import std.runtime.gc

interface TupleReadI { func tag(): Int64 }
interface TupleReadJ <: TupleReadI {}
open class TupleReadA <: TupleReadJ {
    public func tag(): Int64 { 73 }
}
open class TupleReadB <: TupleReadA {}
class TupleReadC <: TupleReadB {}

func castClass<T>(t: (TupleReadB, Int64)): ?T { t as T }
func castInterface<T>(t: (TupleReadB, TupleReadJ, Int64)): ?T { t as T }
func castNested<T>(t: ((TupleReadB, Int64), Int64)): ?T { t as T }

func requireTupleRead(ok: Bool, name: String) {
    if (!ok) { throw Exception("tuple-runtime-read assertion: " + name) }
    println("PASS tuple-runtime-read " + name)
}

main(): Int64 {
    let b = TupleReadB()
    let j: TupleReadJ = TupleReadA()
    let t = (b, 17)
    gc(heavy: true)
    requireTupleRead(match (castClass<(TupleReadC, Int64)>(t)) {
        case None => true
        case Some(_) => false
    }, "class negative")
    let up = castClass<(TupleReadA, Int64)>(t)
    gc(heavy: true)
    requireTupleRead(match (up) {
        case Some(v) => v[0].tag() == 73 && v[1] == 17
        case None => false
    }, "class positive survives GC")
    let ifc = castInterface<(TupleReadA, TupleReadI, Int64)>((b, j, 29))
    gc(heavy: true)
    requireTupleRead(match (ifc) {
        case Some(v) => v[0].tag() == 73 && v[1].tag() == 73 && v[2] == 29
        case None => false
    }, "interface positive survives GC")
    let nested = castNested<((TupleReadA, Int64), Int64)>((t, 31))
    gc(heavy: true)
    requireTupleRead(match (nested) {
        case Some(v) => v[0][0].tag() == 73 && v[0][1] == 17 && v[1] == 31
        case None => false
    }, "nested positive survives GC")
    return 0
}
