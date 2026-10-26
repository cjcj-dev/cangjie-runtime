// Run only with the qualified Windows compiler/runtime/new std tuple.
// Link windows_exception_native.cpp's DLL; execute from install and unpack.
foreign func WindowsCrossPending(callback: CFunc<() -> Unit>): Unit
foreign func WindowsNativeCleanupState(): UInt32

class WindowsTransferError <: Exception {
    public var observed: Int64 = 0
}
let expected = WindowsTransferError()

func raiseExpected(): Unit { throw expected }

func checkTransfer(crossing: Bool): Int64 {
    expected.observed = 0
    var caught = false
    var normalEdge = false
    try {
        if (crossing) {
            let callback: CFunc<() -> Unit> = { => raiseExpected() }
            unsafe { WindowsCrossPending(callback) }
        } else {
            raiseExpected()
        }
        normalEdge = true
    } catch (error: WindowsTransferError) {
        error.observed = 29
        caught = true
    }
    let cleanup = if (crossing) { unsafe { WindowsNativeCleanupState() } } else { 0x202u32 }
    let ok = caught && !normalEdge && expected.observed == 29 && cleanup == 0x202u32
    println("WINDOWS_MANAGED_TRANSFER crossing=${crossing} caught=${caught} normal=${normalEdge} cleanup=${cleanup} pass=${ok}")
    return if (ok) { 0 } else { 1 }
}

func returnNormally(): Unit {}

main(): Int64 {
    var failures = checkTransfer(false) + checkTransfer(true)
    let normal: CFunc<() -> Unit> = { => returnNormally() }
    unsafe { WindowsCrossPending(normal) }
    let normalOk = unsafe { WindowsNativeCleanupState() } == 0x202u32
    println("WINDOWS_MANAGED_NORMAL_CONTROL pass=${normalOk}")
    if (!normalOk) { failures++ }
    // The successful crossing must not leave a stale pending target.
    failures += checkTransfer(true)
    println("WINDOWS_MANAGED_TRANSFER_RESULT failures=${failures}")
    return failures
}
