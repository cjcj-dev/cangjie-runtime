/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 */
package std.core

// java.lang.ref.Reference: the compiler publishes these actual member offsets
// and classifies accesses by the declaring field. The VM never extends objects.
protected abstract class Reference {
    protected var _value: Option<Object>
    var _queue: ReferenceQueue
    var _next: Option<Reference> = None
    var _discovered: Option<Reference> = None

    protected init(value: Option<Object>, queue: ReferenceQueue) {
        _value = value
        _queue = queue
    }
}

protected open class FinalReference <: Reference {
    protected init(value: Object, queue: ReferenceQueue) {
        super(Some(value), queue)
    }

    protected func getInactive(): Option<Object> {
        if (_next.isNone()) {
            throw IllegalStateException("FinalReference is active")
        }
        _value
    }

    protected func clearInactive(): Unit {
        if (_next.isNone()) {
            throw IllegalStateException("FinalReference is active")
        }
        _value = None
    }
}

protected let referenceNullQueue: ReferenceQueue = NullReferenceQueue()
let referenceEnqueuedQueue: ReferenceQueue = NullReferenceQueue()

class NullReferenceQueue <: ReferenceQueue {
    protected override func enqueue(reference: Reference): Bool { false }
}

protected open class ReferenceQueue {
    private var head: Option<Reference> = None
    private var queueLength: Int64 = 0
    private let lock: ReferenceMonitor

    public init() {
        let mutex = ReferenceMutex()
        if (!mutex.initialize()) {
            throw IllegalStateException("ReferenceQueue mutex initialization failed")
        }
        lock = ReferenceMonitor(mutex)
        if (!lock.initialize()) {
            throw IllegalStateException("ReferenceQueue monitor initialization failed")
        }
    }

    private func enqueue0(reference: Reference): Bool {
        if (refEq(reference._queue, referenceNullQueue) || refEq(reference._queue, referenceEnqueuedQueue)) {
            return false
        }
        reference._next = if (head.isNone()) { Some(reference) } else { head }
        head = Some(reference)
        queueLength++
        reference._queue = referenceEnqueuedQueue
        lock.notifyAll()
        true
    }

    protected open func enqueue(reference: Reference): Bool {
        lock.lock()
        try { enqueue0(reference) } finally { lock.unlock() }
    }

    private func poll0(): Option<Reference> {
        if (let Some(reference) <- head) {
            reference._queue = referenceNullQueue
            let next = reference._next.getOrThrow()
            head = if (refEq(next, reference)) { None } else { Some(next) }
            reference._next = Some(reference)
            queueLength--
            return Some(reference)
        }
        None
    }

    func poll(): Option<Reference> {
        if (head.isNone()) { return None }
        lock.lock()
        try { poll0() } finally { lock.unlock() }
    }

    func remove(): Reference {
        lock.lock()
        try {
            while (true) {
                if (let Some(reference) <- poll0()) { return reference }
                lock.wait(Int64.Max)
            }
            throw IllegalStateException("Unreachable reference queue exit")
        } finally { lock.unlock() }
    }
}
