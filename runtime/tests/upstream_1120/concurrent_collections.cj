// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

import std.collection.MapEntryView
import std.collection.concurrent.*
import std.env.exit

func checkMap(throwFromCallback: Bool): Int64 {
    let map = ConcurrentHashMap<Int64, Int64>(64, concurrencyLevel: 2)
    map.add(7, 10)
    let call = spawn {
        try {
            map.entryView(7, { view: MapEntryView<Int64, Int64> =>
                if (view.value != Some(10)) {
                    throw IllegalArgumentException("wrong entry before callback")
                }
                println("CHM_ENTRY_VIEW_CALLBACK_REACHED")
                if (throwFromCallback) {
                    throw IllegalStateException("marker")
                }
                view.value = Some(30)
            })
            !throwFromCallback
        } catch (e: IllegalStateException) {
            throwFromCallback && e.message == "marker"
        }
    }

    if (!call.get(5 * Duration.second)) {
        println("FAIL: callback outcome")
        return 40
    }

    // A distinct CJThread must acquire the same stripe, so recursive locking
    // on the callback's thread cannot hide a missed unlock.
    let writer = spawn { map.add(7, 20) }
    try {
        let previous = writer.get(3 * Duration.second)
        if (previous != Some(if (throwFromCallback) { 10 } else { 30 }) || map.get(7) != Some(20) ||
            map.size != 1) {
            println("FAIL: callback changed map contents or size")
            return 42
        }
    } catch (_: TimeoutException) {
        println("FAIL: same stripe still locked after callback")
        // Avoid leaving the stranded writer running in the deliberately broken arm.
        exit(41)
    }

    println("CHM_SAME_STRIPE_ASSERTION_PASS")
    return 0
}

func checkQueues(): Int64 {
    let bounded = ArrayBlockingQueue<Int64>(1)
    bounded.add(1)
    if (bounded.add(2, 25 * Duration.millisecond) || bounded.remove() != 1) {
        println("FAIL: full queue did not preserve its element through timeout")
        return 43
    }
    if (bounded.remove(25 * Duration.millisecond).isSome()) {
        println("FAIL: empty queue returned a value")
        return 44
    }
    if (!bounded.add(3, Duration.second) || bounded.remove(Duration.second) != Some(3)) {
        println("FAIL: timed queue operations did not resume")
        return 45
    }

    let unbounded = ConcurrentLinkedQueue<Int64>()
    for (i in 0..129) {
        unbounded.add(i)
    }
    var next = 0
    for (value in unbounded) {
        if (value != next) {
            println("FAIL: iterator lost queue order")
            return 46
        }
        next++
    }
    if (next != 129) {
        println("FAIL: iterator stopped before all completed blocks")
        return 47
    }
    println("QUEUE_ASSERTIONS_PASS")
    return 0
}

main(args: Array<String>): Int64 {
    match (args[0]) {
        case "throw" => checkMap(true)
        case "normal" => checkMap(false)
        case "queue" => checkQueues()
        case _ => 48
    }
}
