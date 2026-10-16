// Managed producer/consumer regression draft for #1322.
import std.runtime.*

class GCTotalsPayload {
    let bytes: Array<UInt8>
    init(value: UInt8) {
        bytes = Array<UInt8>(1024 * 1024, repeat: value)
    }
}

func garbageWave(seed: UInt8): Int64 {
    var slots = Array<?GCTotalsPayload>(8, {_ => None})
    var checksum: Int64 = 0
    for (i in 0..8) {
        slots[i] = GCTotalsPayload(seed)
    }
    for (i in 0..8) {
        if (let Some(payload) <- slots[i]) {
            checksum += Int64(payload.bytes[1024 * 1024 - 1])
        }
        slots[i] = None
    }
    return checksum
}

main(): Int64 {
    var previousTime = getGCTime()
    var previousFreed = getGCFreedSize()
    var checksum: Int64 = 0
    var failed = false
    println("GC_TOTALS_START time=${previousTime} freed=${previousFreed}")
    for (round in 1..4) {
        checksum += garbageWave(UInt8(round))
        let countBefore = getGCCount()
        gc(heavy: true)
        let countAfter = getGCCount()
        let time = getGCTime()
        let freed = getGCFreedSize()
        let timeOK = time > 0 && time >= previousTime
        let freedOK = freed > 0 && freed >= previousFreed
        // Evaluate and print both target assertions before returning a failure.
        println("GC_TOTALS_TARGET round=${round} count_before=${countBefore} count_after=${countAfter} time=${time} freed=${freed} time_ok=${timeOK} freed_ok=${freedOK}")
        failed = failed || !timeOK || !freedOK
        previousTime = time
        previousFreed = freed
    }
    println("GC_TOTALS_FINISH checksum=${checksum} failed=${failed}")
    return if (failed) { 1 } else { 0 }
}
