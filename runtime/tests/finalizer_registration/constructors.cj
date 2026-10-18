// Real language construction, including both sides of Object.init return.
import std.runtime.*

foreign func FinalizerRegistrationObserved(kind: Int32): Unit
foreign func FinalizerRegistrationCheckpoint(stage: Int32): Unit
foreign func FinalizerRegistrationCounts(): Int64

open class ConstructorBase {
    init(_: Int64) {}
}

func failBeforeRoot(): Int64 {
    throw Exception("before root constructor")
}

class NormalConstructor <: ConstructorBase {
    init() { super(0) }
    ~init() { unsafe { FinalizerRegistrationObserved(0) } }
}

class BeforeRootConstructor <: ConstructorBase {
    init() { super(failBeforeRoot()) }
    ~init() { unsafe { FinalizerRegistrationObserved(1) } }
}

class AfterRootConstructor <: ConstructorBase {
    init() {
        super(0)
        throw Exception("after root constructor")
    }
    ~init() { unsafe { FinalizerRegistrationObserved(2) } }
}

func constructCases(): Bool {
    unsafe { FinalizerRegistrationCheckpoint(0) }
    let _ = NormalConstructor()
    unsafe { FinalizerRegistrationCheckpoint(1) }
    var beforeCaught = false
    try {
        let _ = BeforeRootConstructor()
    } catch (_: Exception) {
        beforeCaught = true
    }
    unsafe { FinalizerRegistrationCheckpoint(2) }
    var afterCaught = false
    try {
        let _ = AfterRootConstructor()
    } catch (_: Exception) {
        afterCaught = true
    }
    unsafe { FinalizerRegistrationCheckpoint(3) }
    return beforeCaught && afterCaught
}

main(): Int64 {
    if (!constructCases()) { return 2 }
    // Reference processing and the managed finalizer worker are asynchronous.
    // Keep the existing two-collection finalizer-trigger protocol; the native
    // observer checks exact counts rather than accepting process completion.
    for (_ in 0..2) {
        gc(heavy: true)
        sleep(500 * Duration.millisecond)
    }
    return unsafe { FinalizerRegistrationCounts() }
}
