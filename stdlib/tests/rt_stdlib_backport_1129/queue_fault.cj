// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.

package std.collection.concurrent

import std.env.exit
import std.sync.*

private let fault = AtomicInt64(0)
private let allocating = AtomicBool(false)
private let waiting = AtomicBool(false)

private func checkQueue(condition: Bool, message: String): Unit {
    if (!condition) {
        eprintln("FAIL: ${message}")
        exit(1)
    }
}

private func waitFor(flag: AtomicBool, message: String): Unit {
    for (_ in 0..3000) {
        if (flag.load()) {
            return
        }
        sleep(Duration.millisecond)
    }
    checkQueue(false, message)
}

// The runner inserts these two calls into a copy of the real package.
// The contender must reach the losing CAS branch before the winner fails.
func observeQueueGrowthWaiter(): Unit {
    waiting.store(true)
}

func failQueueGrowthAllocation(): Unit {
    let mode = fault.load()
    if (mode == 0 || !fault.compareAndSwap(mode, 0)) {
        return
    }
    allocating.store(true)
    if (mode == 2) {
        waitFor(waiting, "linked contender did not reach the losing CAS")
        println("linked contender entered the wait branch before allocation failed")
    }
    // The byte count overflows the allocator's size representation, so it
    // raises OutOfMemoryError before reserving memory. The raw-data use keeps
    // the compiler from discarding the allocation as an unused value.
    let impossible = Array<UInt64>(Int64.Max, repeat: 0)
    unsafe {
        let raw = acquireArrayRawData(impossible)
        releaseArrayRawData(raw)
    }
    checkQueue(false, "injected allocation unexpectedly succeeded")
}

public func checkBlockingQueueGrowthRecovery(): Unit {
    let queue = BlockingQueue<Int64>(BUF_SIZE + 2)
    for (i in 0..BUF_SIZE) {
        queue.add(i)
    }
    fault.store(1)
    var failed = false
    try {
        queue.add(-1)
    } catch (_: OutOfMemoryError) {
        failed = true
    }
    checkQueue(failed && allocating.load(), "blocking growth did not propagate the injected allocation error")
    checkQueue(queue.size == BUF_SIZE, "blocking count changed after failed growth")
    println("blocking failure propagated with full-block count intact")
    let successor = spawn { queue.add(BUF_SIZE) }
    try {
        successor.get(Duration.second * 3)
    } catch (_: TimeoutException) {
        checkQueue(false, "blocking successor did not recover after failed growth")
    }
    checkQueue(queue.size == BUF_SIZE + 1, "blocking count after successor")
    for (i in 0..=BUF_SIZE) {
        checkQueue(queue.remove() == i, "blocking FIFO at ${i}")
    }
    checkQueue(queue.size == 0 && queue.tryRemove().isNone(), "blocking empty after draining")
    println("PASS: blocking serial growth recovery and FIFO")
}

public func checkLinkedQueueGrowthRecovery(): Unit {
    let queue = ConcurrentLinkedQueue<Int64>()
    for (i in 0..BUF_SIZE) {
        checkQueue(queue.add(i), "linked fill at ${i}")
    }
    fault.store(2)
    let winner = spawn {
        try {
            queue.add(-1)
            false
        } catch (_: OutOfMemoryError) {
            true
        }
    }
    waitFor(allocating, "linked winner did not reach the allocation")
    let contender = spawn { queue.add(BUF_SIZE) }
    checkQueue(winner.get(Duration.second * 5), "linked winner did not propagate the injected allocation error")
    try {
        checkQueue(contender.get(Duration.second * 3), "linked contender returned false")
    } catch (_: TimeoutException) {
        checkQueue(false, "linked contender did not reclaim the failed extension")
    }
    checkQueue(queue.size == BUF_SIZE + 1, "linked size after concurrent recovery")
    for (i in 0..=BUF_SIZE) {
        checkQueue(queue.remove() == Some(i), "linked FIFO at ${i}")
    }
    checkQueue(queue.size == 0 && queue.remove().isNone(), "linked empty after draining")
    println("PASS: linked waiting producer reclaimed growth and preserved FIFO")
}
