import std.runtime.*

foreign {
    func NoFPContext(): Int32
    func FPContext(control: Float64): Int32
    func NoFPZero(): Int64
    func NoFPOne(x: Int64): Int64
    func NoFPSix(a: Int64,b: Int64,c: Int64,d: Int64,e: Int64,f: Int64): Int64
    func NoFPPointer(p: CPointer<Unit>): CPointer<Unit>
    func NoFPVoid(): Unit
    func FPControl(x: Float64): Float64
    func StackControl(a: Int64,b: Int64,c: Int64,d: Int64,e: Int64,f: Int64,g: Int64): Int64
    func NoFPReset(): Unit
    func NoFPWaitStarted(): Int32
    func NoFPComplete(): Unit
    func NoFPWaitGC(): Int32
    func FPWaitGC(control: Float64): Int32
    func NoFPCallback(callback: CFunc<() -> Unit>): Unit
    func NoFPCleaned(): Int32
}
func requireNoFP(ok: Bool, name: String): Unit {
    if (!ok) { throw Exception("NOFP_ASSERT " + name) }
}
class LiveRoot {
    let payload = Array<Int64>(4096, {i => i * 7 + 11})
    func check(): Bool { payload[0] == 11 && payload[4095] == 28676 }
}
class CallbackError <: Exception { public var observed: Int64 = 0 }
let expected = CallbackError()
func callbackWork(): Unit {
    let root = LiveRoot()
    let before = getGCCount()
    gc(heavy: true)
    requireNoFP(getGCCount() > before && root.check(), "callback-gc-root")
    throw expected
}
// The foreign producer prevents constant folding of the live FP value.
func liveFloating(input: Float64): Float64 {
    let live = unsafe { FPControl(input) }
    let value = unsafe { NoFPOne(17) }
    requireNoFP(value == 52, "one-arg-integer-result")
    return live + 3.25
}
func progress(control: Bool): Unit {
    unsafe { NoFPReset() }
    let root = LiveRoot()
    let worker = spawn { =>
        requireNoFP(unsafe { NoFPWaitStarted() } == 1, "native-started")
        let before = getGCCount()
        gc(heavy: true)
        requireNoFP(getGCCount() > before, "actual-gc-counter")
        unsafe { NoFPComplete() }
    }
    let result = if (control) { unsafe { FPWaitGC(1.25) } } else { unsafe { NoFPWaitGC() } }
    // Capture while in native; joining afterwards must not conceal a stall.
    let progressed = result == 1
    worker.get()
    requireNoFP(root.check(), "root-after-native")
    requireNoFP(progressed, if (control) { "fp-control-progress" } else { "nofp-native-progress" })
    println("NOFP_PROGRESS control=${control}")
}
main(args: Array<String>): Int64 {
    requireNoFP(unsafe { FPContext(1.25) } == 7, "fp-control-canonical-pc-safe-frame")
    requireNoFP(unsafe { NoFPContext() } == 7, "nofp-canonical-pc-safe-frame")
    progress(true)
    progress(false)
    requireNoFP(liveFloating(1.25) == 6.25, "live-fp-across-clobber")
    requireNoFP(unsafe { NoFPZero() } == 29, "zero-arg-result")
    requireNoFP(unsafe { NoFPSix(1,2,3,4,5,6) } == 91, "six-arg-result")
    requireNoFP(unsafe { StackControl(1,2,3,4,5,6,7) } == 28, "stack-control")
    let p = CPointer<Unit>()
    requireNoFP(unsafe { NoFPPointer(p) }.toUIntNative() == p.toUIntNative(), "pointer-result")
    unsafe { NoFPVoid() }
    var caught = false
    var normal = false
    let callback: CFunc<() -> Unit> = { => callbackWork() }
    try {
        unsafe { NoFPCallback(callback) }
        normal = true
    } catch (e: CallbackError) {
        e.observed = 43
        caught = true
    }
    requireNoFP(caught && !normal && expected.observed == 43 && unsafe { NoFPCleaned() } == 37,
        "callback-exception-identity-cleanup")
    println("C2N_NOFP_OK")
    return 0
}
