foreign func RevSeedExposure(): Unit
foreign func RevReportExposure(): UInt64

class Box {
    var n: Int64
    init(v: Int64) { n = v }
}

func deep(k: Int64, acc: Box): Unit {
    let live = acc.n
    if (k <= 0) {
        throw Exception()
    }
    deep(k - 1, Box(k + live))
}

main(): Int64 {
    unsafe { RevSeedExposure() }
    try {
        deep(16, Box(0))
    } catch (_: Exception) {}
    let summary = unsafe { RevReportExposure() }
    let processOne = summary >> 32
    let visitorHits = summary & 0xffffffff
    println("PRODUCT_ENTRY_DONE summary=${summary} processOne=${processOne} visitorHits=${visitorHits}")
    return if (processOne > 0 && visitorHits > 0) { 0 } else { 41 }
}
