foreign func RevSeedExposure(): Unit
foreign func RevReportExposure(): UInt64

main(): Int64 {
    unsafe { RevSeedExposure() }
    try {
        throw Exception()
    } catch (_: Exception) {}
    let summary = unsafe { RevReportExposure() }
    println("PRODUCT_ENTRY_DONE summary=${summary}")
    return if (summary > 0) { 0 } else { 41 }
}
