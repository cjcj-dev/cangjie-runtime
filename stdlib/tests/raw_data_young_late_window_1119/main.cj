// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.

// acquireArrayRawData while young relocation sets are being selected: holders
// acquire arrays on sparse young pages at any time, block in C (lw_native.c)
// while holding, and check before releaseArrayRawData that the second C write
// through the same pointer is visible through the array. An array that moved
// while held fails that check, or the release (no matching acquire). Allocation
// churn on a large heap keeps young selections long and frequent, so acquires
// land inside them, before and after their final scan.
// args: seconds holders usec
import std.collection.*
import std.convert.*
import std.random.*
import std.runtime.*
import std.sync.*
import std.time.*

foreign func lw_fill(p: CPointer<Int64>, n: Int64, v: Int64, usec: Int64): Int64

let stop = AtomicBool(false)
let failures = AtomicInt64(0)

main(args: Array<String>): Int64 {
    let seconds: Int64 = if (args.size > 0) { Int64.parse(args[0]) } else { 10 }
    let holders: Int64 = if (args.size > 1) { Int64.parse(args[1]) } else { 8 }
    let usec: Int64 = if (args.size > 2) { Int64.parse(args[2]) } else { 2000 }
    let futures = ArrayList<Future<Int64>>()
    for (t in 0..holders) {
        futures.add(spawn {
            let rnd = Random(UInt64(t + 1))
            let pool = Array<Array<Int64>>(1024, {_ => Array<Int64>(64, repeat: 0)})
            var iters: Int64 = 0
            var maxNs: Int64 = 0
            var stamp: Int64 = t * 1000000000
            while (!stop.load()) {
                let j = Int64(rnd.nextUInt64() % 1024)
                if (iters % 4 == 0) {
                    pool[j] = Array<Int64>(64, repeat: 0)
                }
                let a = pool[j]
                stamp++
                let t0 = MonoTime.now()
                unsafe {
                    let h = acquireArrayRawData(a)
                    let d = (MonoTime.now() - t0).toNanoseconds()
                    if (d > maxNs) {
                        maxNs = d
                    }
                    lw_fill(h.pointer, 64, stamp, usec)
                    for (i in 0..64) {
                        if (a[i] != stamp + 2 * i) {
                            failures.fetchAdd(1)
                            println("LATEWIN_MISMATCH t=${t} i=${i} expected=${stamp + 2 * i} got=${a[i]}")
                            break
                        }
                    }
                    releaseArrayRawData(h)
                }
                iters++
            }
            println("HOLDER t=${t} iters=${iters} max_acquire_ms=${maxNs / 1000000}")
            iters
        })
    }
    let slots: Int64 = 2000000
    let table = Array<Array<Int64>>(slots, {i => Array<Int64>(64, repeat: i)})
    let rnd = Random(12345)
    let t0 = MonoTime.now()
    var allocs: Int64 = 0
    while (true) {
        for (k in 0..256) {
            let a = Array<Int64>(64, repeat: allocs)
            allocs++
            if (k == 0) {
                table[Int64(rnd.nextUInt64() % UInt64(slots))] = a
            }
        }
        if (allocs % (256 * 2048) == 0 && (MonoTime.now() - t0).toMilliseconds() >= seconds * 1000) {
            break
        }
    }
    stop.store(true)
    var total: Int64 = 0
    for (f in futures) {
        total += f.get()
    }
    let failed = failures.load()
    if (failed != 0) {
        println("LATEWIN_FAIL failures=${failed} iters=${total} gc=${getGCCount()}")
        return 1
    }
    println("LATEWIN_PASS iters=${total} gc=${getGCCount()} table=${table.size}")
    return 0
}
