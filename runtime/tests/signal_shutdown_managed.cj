package shutdownprobe

import std.collection.ArrayList

foreign func ShutdownInstallCallback(callback: CFunc<(Int32, CPointer<Unit>, CPointer<Unit>) -> Bool>): Unit
foreign func ShutdownInstallGcCallback(callback: CFunc<(Int32, CPointer<Unit>, CPointer<Unit>) -> Bool>): Unit
foreign func ShutdownNativeBlock(): Unit
foreign func ShutdownManagedProgress(stage: Int32): Unit
foreign func ShutdownManagedAwaitGc(): Int64
foreign func ShutdownManagedReport(bad: Int64): Unit

@C
func shutdownManagedCallback(_: Int32, _: CPointer<Unit>, _: CPointer<Unit>): Bool {
    unsafe { ShutdownManagedProgress(1) }
    unsafe { ShutdownNativeBlock() }
    unsafe { ShutdownManagedProgress(2) }
    return true
}

// Keep the batch reachable from a package root for the whole callback, so a
// world-stop that misses this frame shows up as corrupted contents rather than
// as a silently freed batch.
var managedCells: ArrayList<Int64> = ArrayList<Int64>()

@C
func shutdownManagedGcCallback(_: Int32, _: CPointer<Unit>, _: CPointer<Unit>): Bool {
    let cells = ArrayList<Int64>()
    for (index in 0..256) {
        cells.add(index * 3 + 1)
    }
    managedCells = cells
    unsafe { ShutdownManagedProgress(1) }
    // Managed code, runnable and polling: the host requests a real collection
    // while this frame is live. The dispatcher calls handlers in the state it
    // already has (os.cpp:434-441), so this frame is suspended and resumed like
    // any other mutator frame.
    var waited: Int64 = 0
    while (waited == 0) {
        waited = unsafe { ShutdownManagedAwaitGc() }
    }
    var bad: Int64 = 0
    for (index in 0..256) {
        if (cells[index] != index * 3 + 1) {
            bad += 1
        }
    }
    unsafe { ShutdownManagedReport(bad) }
    unsafe { ShutdownManagedProgress(2) }
    return true
}

func installCallback(): Int64 {
    unsafe { ShutdownInstallCallback(shutdownManagedCallback) }
    unsafe { ShutdownInstallGcCallback(shutdownManagedGcCallback) }
    return 1
}

public let shutdownInstalled: Int64 = installCallback()
