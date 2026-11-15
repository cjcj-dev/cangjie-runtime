foreign {
    func FPControl(x: Float64): Float64
    func NoFPZero(): Int64
}
main(): Int64 {
    if (unsafe { FPControl(1.25) } != 3.0) { throw Exception("NOFP_GUARD_ASSERT fp-control") }
    println("NOFP_GUARD_FP_CONTROL_OK")
    try {
        let result = unsafe { NoFPZero() }
        if (result != 29) { throw Exception("NOFP_GUARD_ASSERT normal-result") }
    } catch (e: StackOverflowError) {
        println("NOFP_GUARD_TARGET overflow")
        return 7
    }
    println("NOFP_GUARD_TARGET normal")
    return 0
}
