// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.

// acquireArrayRawData: a C write through the acquired pointer is visible to
// Cangjie before releaseArrayRawData, while the holder is blocked in a foreign
// call and heavy collections complete. The order is fixed by a native handshake
// (rd_native.c), not by sleeps, and every check runs before the release:
//   stage 1: C wrote s1        -> Cangjie reads s1 (fails copy-back-on-release)
//   heavy GCs with churn complete while the holder is still blocked in C
//            (fails a collector that waits for the holder)
//   stage 3: C wrote s2 through the same pointer -> Cangjie reads s2 through the
//            array and an overlapping slice (fails if the array moved)
//   stage 4: only now may the holder release.
import std.runtime.*

foreign func rd_hold(p: CPointer<Int64>, idx: Int64, s1: Int64, s2: Int64): Int64
foreign func rd_wait_stage(s: Int64): Unit
foreign func rd_set_stage(s: Int64): Unit

var sink: Array<Array<Int64>> = Array<Array<Int64>>()

main(): Int64 {
    // Allocate the target among short-lived arrays so its page is sparse and a
    // normal cycle would select it for relocation.
    var keep = Array<Int64>(0, repeat: 0)
    for (i in 0..8192) {
        let a = Array<Int64>(64, repeat: 0)
        if (i == 4096) {
            keep = a
        }
    }
    let data = keep
    gc(heavy: true)
    let idx = 37
    let s1: Int64 = 0x5151_5151
    let s2: Int64 = 0x7272_7272
    let holder = spawn {
        unsafe {
            let h = acquireArrayRawData(data)
            let addr = rd_hold(h.pointer, idx, s1, s2)
            releaseArrayRawData(h)
            addr
        }
    }
    unsafe { rd_wait_stage(1) }
    let v1 = data[idx]
    if (v1 != s1) {
        println("RAWDATA_FAIL stage1 expected=${s1} got=${v1}")
        unsafe { rd_set_stage(4) }
        holder.get()
        return 2
    }
    for (round in 0..3) {
        for (i in 0..20000) {
            sink = Array<Array<Int64>>(4, {j => Array<Int64>(32, repeat: i + j + round)})
        }
        gc(heavy: true)
    }
    println("RAWDATA_GC_COMPLETED_WHILE_HELD rounds=3")
    unsafe {
        rd_set_stage(2)
        rd_wait_stage(3)
    }
    let v2 = data[idx]
    let view = data[idx - 1..idx + 2]
    let v3 = view[1]
    unsafe { rd_set_stage(4) }
    let addr = holder.get()
    if (v2 != s2 || v3 != s2) {
        println("RAWDATA_FAIL stage3 expected=${s2} array=${v2} slice=${v3}")
        return 3
    }
    println("RAWDATA_PASS s1=${v1} s2=${v2} slice=${v3} addr=${addr}")
    return 0
}
