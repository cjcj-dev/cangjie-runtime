/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 */
package std.core

// The existing VM synchronization layouts live in the foundational package.
// No language fields may be added to the Mutex carrier: the backend supplies
// the runtime's Mutex payload. Both std.sync and reference processing use it.
protected class ReferenceMutex {
    public init() {}
    public func initialize(): Bool { !mutexInit(this) }
    public func lock(): Unit { mutexLock(this) }
    public func tryLock(): Bool { mutexTryLock(this) }
    public func isOwned(): Bool { mutexCheckStatus(this) }
    public func unlock(): Unit { mutexUnlock(this) }
}

protected class ReferenceMonitor {
    private let mutex: ReferenceMutex

    public init(mutex: ReferenceMutex) { this.mutex = mutex }
    public func initialize(): Bool { !monitorInit(this) }
    public func lock(): Unit { mutex.lock() }
    public func unlock(): Unit { mutex.unlock() }
    public func wait(timeout: Int64): Bool { monitorWait(this, timeout) }
    public func notify(): Unit { monitorNotify(this) }
    public func notifyAll(): Unit { monitorNotifyAll(this) }
}

@Intrinsic
func mutexInit(mutex: ReferenceMutex): Bool
@Intrinsic
func mutexLock(mutex: ReferenceMutex): Unit
@Intrinsic
func mutexTryLock(mutex: ReferenceMutex): Bool
@Intrinsic
func mutexCheckStatus(mutex: ReferenceMutex): Bool
@Intrinsic
func mutexUnlock(mutex: ReferenceMutex): Unit
@Intrinsic
func monitorInit(monitor: ReferenceMonitor): Bool
@Intrinsic
func monitorWait(monitor: ReferenceMonitor, timeout: Int64): Bool
@Intrinsic
func monitorNotify(monitor: ReferenceMonitor): Unit
@Intrinsic
func monitorNotifyAll(monitor: ReferenceMonitor): Unit
