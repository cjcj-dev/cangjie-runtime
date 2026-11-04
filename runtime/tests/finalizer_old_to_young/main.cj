// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
import std.ref.*
import std.runtime.*
import std.sync.*

foreign func R11CollectYoung(): Int64

let r11Entered = AtomicBool(false)
let r11Release = AtomicBool(false)
let r11Done = AtomicBool(false)
let r11Count = AtomicInt64(0)
let r11Value = AtomicInt64(0)
let r11TimedOut = AtomicBool(false)

class R11Payload {
    let value: Int64
    init(value: Int64) { this.value = value }
}

class R11Owner {
    var child: ?R11Payload = None
    init() {}
    ~init() {
        // Old reference processing reached this owner before allocating the
        // child. Only this old field retains the child across the young GCs.
        child = Some(R11Payload(1105))
        r11Count.fetchAdd(1)
        r11Entered.store(true)
        for (_ in 0..400) {
            if (r11Release.load()) { break }
            sleep(25 * Duration.millisecond)
        }
        if (!r11Release.load()) { r11TimedOut.store(true) }
        r11Value.store(child.getOrThrow().value)
        r11Done.store(true)
    }
}

func r11Allocate(): WeakRef<R11Owner> {
    WeakRef<R11Owner>(R11Owner(), EAGER)
}

main(): Int64 {
    let weak = r11Allocate()
    let control = R11Payload(7)
    gc(heavy: true)
    for (_ in 0..200) {
        if (r11Entered.load()) { break }
        sleep(25 * Duration.millisecond)
    }
    if (!r11Entered.load()) {
        r11Release.store(true)
        println("R11_FAIL finalizer_not_entered")
        return 10
    }
    var minorOk = true
    // gc(heavy: false) currently also requests a major GC. The test-only
    // native bridge submits GC_REASON_YOUNG through the normal request path.
    for (round in 0..3) {
        let rc = unsafe { R11CollectYoung() }
        println("R11_YOUNG round=${round} rc=${rc}")
        if (rc != 0) { minorOk = false }
    }
    r11Release.store(true)
    for (_ in 0..200) {
        if (r11Done.load()) { break }
        sleep(25 * Duration.millisecond)
    }
    let passed = minorOk && r11Done.load() && !r11TimedOut.load() &&
        r11Count.load() == 1 && r11Value.load() == 1105 &&
        weak.value.isNone() && control.value == 7
    println("R11_ASSERT done=${r11Done.load()} count=${r11Count.load()} child=${r11Value.load()} control=${control.value} pass=${passed}")
    if (passed) { 0 } else { 11 }
}
