import reference_startup_dependency.*
import std.runtime.*
import std.sync.*

main(): Int64 {
    gc(heavy: true)
    gc(heavy: true)
    for (_ in 0..200) {
        if (startupFinalized.load() != 0) { break }
        sleep(25 * Duration.millisecond)
    }
    let finalized = startupFinalized.load() == 1356
    let cleared = startupWeak.value.isNone()
    println("REFERENCE1356_STARTUP_TARGET finalized=${finalized} cleared=${cleared}")
    if (finalized && cleared) { 0 } else { 1 }
}
