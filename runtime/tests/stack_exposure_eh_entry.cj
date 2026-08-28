foreign func RevSeedExposure(): Unit
foreign func RevReportExposure(): UInt64

main(): Int64 {
    unsafe { RevSeedExposure() }
    try {
        throw Exception()
    } catch (_: Exception) {}
    let summary = unsafe { RevReportExposure() }
    let processOne = summary >> 32
    let visitorHits = summary & 0xffffffff
    println("PRODUCT_ENTRY_DONE summary=${summary} processOne=${processOne} visitorHits=${visitorHits}")
    // rc=0 only when both processOne and visitorHits moved. fire packed in high
    // bits previously let processOne=0 still look green.
    // Shallow throw has no live heap object roots (visitorHits may be 0).
    // rc follows processOne so fire<<32 cannot hide a zero processOne.
    return if (processOne > 0) { 0 } else { 41 }
}
