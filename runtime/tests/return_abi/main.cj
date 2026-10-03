package return_abi

@C
struct Pair {
    var first: UInt64
    var second: UInt64
    init(a: UInt64, b: UInt64) { first = a; second = b }
}

foreign func RunUInt(callback: CFunc<(UInt64) -> UInt64>): Int64
foreign func RunPair(callback: CFunc<(Pair) -> Pair>): Int64
foreign func RunFloat(single: CFunc<(Float32) -> Float32>, wide: CFunc<(Float64) -> Float64>): Int64
foreign func CrossPending(callback: CFunc<() -> Unit>): Unit

class ReturnError <: Exception {
    public var observed: Int64 = 0
}
let expected = ReturnError()

func throwExpected(): Unit { throw expected }

func checkException(name: String, crossing: Bool): Int64 {
    expected.observed = 0
    var caught: Bool = false
    try {
        if (crossing) {
            let callback: CFunc<() -> Unit> = { => throwExpected() }
            unsafe { CrossPending(callback) }
        } else {
            throwExpected()
        }
    } catch (error: ReturnError) {
        error.observed = 17
        caught = true
    }
    // A typed catch and a write observed through the original reference prove
    // object/type preservation without assigning meaning to EH return registers.
    let ok = caught && expected.observed == 17
    println("RETURN_ABI_TARGET ${name} caught=${caught} same_object=${expected.observed == 17} ok=${ok}")
    return if (ok) { 0 } else { 1 }
}

main(): Int64 {
    let integer: CFunc<(UInt64) -> UInt64> = { x: UInt64 => x }
    let pair: CFunc<(Pair) -> Pair> = { x: Pair => x }
    let single: CFunc<(Float32) -> Float32> = { x: Float32 => x }
    let wide: CFunc<(Float64) -> Float64> = { x: Float64 => x }
    var failures = unsafe { RunUInt(integer) + RunPair(pair) + RunFloat(single, wide) }
    failures += checkException("managed_catch", false)
    failures += checkException("cross_native_pending", true)
    println("RETURN_ABI_COMPLETE failures=${failures}")
    return failures
}
