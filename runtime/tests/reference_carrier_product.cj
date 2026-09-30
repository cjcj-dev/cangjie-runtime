// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
import std.ref.*
import std.runtime.*
import std.sync.*

let finalized = AtomicInt64(0)
let finalizedValue = AtomicInt64(0)

class ReferencePayload {
    let value: Int64
    init(value: Int64) { this.value = value }
}

class CarrierFinalizable {
    let child: ReferencePayload
    init(value: Int64) { child = ReferencePayload(value) }
    ~init() {
        finalizedValue.store(child.value)
        finalized.fetchAdd(1)
    }
}

// Returning only the WeakRef ensures the referent has no mutator root after
// this frame returns. The separate child checks finalizable graph traversal.
func allocateCarrier(): WeakRef<CarrierFinalizable> {
    WeakRef<CarrierFinalizable>(CarrierFinalizable(1356), EAGER)
}

class CarrierChecks {
    private var failures = 0
    func check(name: String, result: Bool): Unit {
        println("REFERENCE1356_TARGET name=${name} pass=${result}")
        if (!result) { failures++ }
    }
    func result(): Int64 { failures }
}

main(): Int64 {
    let checks = CarrierChecks()
    let mutex = Mutex()
    mutex.lock()
    checks.check("mutex_reentrant", mutex.tryLock())
    mutex.unlock()
    mutex.unlock()
    var rejected = false
    try { mutex.unlock() } catch (_: IllegalSynchronizationStateException) { rejected = true }
    checks.check("mutex_ownership", rejected)

    let monitor = Monitor()
    monitor.lock()
    checks.check("monitor_reentrant", monitor.tryLock())
    monitor.notify()
    monitor.notifyAll()
    monitor.unlock()
    monitor.unlock()
    rejected = false
    try { monitor.notify() } catch (_: IllegalSynchronizationStateException) { rejected = true }
    checks.check("monitor_ownership", rejected)

    let retained = ReferencePayload(7)
    let strongWeak = WeakRef<ReferencePayload>(retained, EAGER)
    let weak = allocateCarrier()
    gc(heavy: true)
    gc(heavy: true)
    // Wait for one asynchronous finalizer; no repeated collections to obtain
    // a passing result, and every target below executes even after failure.
    for (_ in 0..200) {
        if (finalized.load() != 0) { break }
        sleep(25 * Duration.millisecond)
    }
    checks.check("weak_cleared", weak.value.isNone())
    checks.check("finalizer_once", finalized.load() == 1)
    checks.check("finalizable_child", finalizedValue.load() == 1356)
    checks.check("strong_referent", strongWeak.value.getOrThrow().value == retained.value)
    gc(heavy: true)
    sleep(100 * Duration.millisecond)
    checks.check("finalizer_not_repeated", finalized.load() == 1)
    strongWeak.clear()
    checks.check("explicit_clear", strongWeak.value.isNone())
    println("REFERENCE1356_RESULT failures=${checks.result()}")
    checks.result()
}
