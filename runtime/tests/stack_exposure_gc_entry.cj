foreign func RevSeedExposure(): Unit
foreign func RevFireGcWalk(): Unit
foreign func RevReportExposure(): UInt64

main(): Int64 {
    unsafe { RevSeedExposure() }
    unsafe { RevFireGcWalk() }
    let summary = unsafe { RevReportExposure() }
    let processOne = summary >> 32
    let visitorHits = summary & 0xffffffff
    println("PRODUCT_ENTRY_DONE summary=${summary} processOne=${processOne} visitorHits=${visitorHits}")
    return if (processOne > 0) { 0 } else { 41 }
}
