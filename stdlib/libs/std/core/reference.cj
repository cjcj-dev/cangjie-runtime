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

    private func remove0(timeout: Int64): Option<Reference> {
        if (let Some(reference) <- poll0()) { return Some(reference) }
        var remaining = timeout
        var start = referenceNanoTime()
        while (true) {
            lock.wait(remaining)
            if (let Some(reference) <- poll0()) { return Some(reference) }
            let end = referenceNanoTime()
            remaining -= end - start
            if (remaining <= 0) { return None }
            start = end
        }
        None
    }

    func remove(timeout: Int64): Option<Reference> {
        lock.lock()
        try { remove0(timeout) } finally { lock.unlock() }
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

private let finalizerQueue = ReferenceQueue()
private let finalizerLock = createFinalizerLock()
private var unfinalized: Option<Finalizer> = None

private func createFinalizerLock(): ReferenceMutex {
    let lock = ReferenceMutex()
    if (!lock.initialize()) { throw IllegalStateException("Finalizer mutex initialization failed") }
    lock
}

class Finalizer <: FinalReference {
    private var registrationNext: Option<Finalizer> = None
    private var registrationPrev: Option<Finalizer> = None

    init(finalizee: Object) {
        super(finalizee, finalizerQueue)
        finalizerLock.lock()
        try {
            if (let Some(previous) <- unfinalized) {
                registrationNext = Some(previous)
                previous.registrationPrev = Some(this)
            }
            unfinalized = Some(this)
        } finally { finalizerLock.unlock() }
    }

    func runFinalizer(): Unit {
        finalizerLock.lock()
        try {
            if (let Some(next) <- registrationNext) {
                if (refEq(next, this)) { return }
            }
            if (let Some(first) <- unfinalized) {
                if (refEq(first, this)) {
                    unfinalized = registrationNext
                } else {
                    registrationPrev.getOrThrow().registrationNext = registrationNext
                }
            }
            if (let Some(next) <- registrationNext) {
                next.registrationPrev = registrationPrev
            }
            registrationPrev = None
            registrationNext = Some(this)
        } finally { finalizerLock.unlock() }
        try {
            referenceInvokeFinalize(getInactive().getOrThrow())
        } catch (_: Exception) {
        } catch (_: Error) {
        } finally { clearInactive() }
    }
}

// Retained and published by the compiler alongside the core runtime bridges.
func registerReferenceFinalizer(finalizee: Object): Unit { Finalizer(finalizee) }

func runReferenceHandler(): Unit {
    while (referenceRuntimeRunning()) {
        var pending = referenceWaitPending()
        while (let Some(reference) <- pending) {
            pending = reference._discovered
            reference._discovered = None
            let queue = reference._queue
            if (!refEq(queue, referenceNullQueue)) { queue.enqueue(reference) }
        }
    }
}

func runReferenceFinalizers(): Unit {
    while (referenceRuntimeRunning()) {
        if (let Some(reference) <- finalizerQueue.remove(1_000_000_000)) {
            (reference as Finalizer).getOrThrow().runFinalizer()
        }
    }
}

@Intrinsic
func referenceInvokeFinalize(finalizee: Object): Unit
@Intrinsic
func referenceWaitPending(): Option<Reference>
@Intrinsic
func referenceRuntimeRunning(): Bool
@Intrinsic
func referenceNanoTime(): Int64
