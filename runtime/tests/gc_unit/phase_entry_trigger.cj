// End-to-end minor forwarding-carrier entry regression.  The bounded allocation
// wave crosses the product young-region threshold; the shell harness requires a
// minor cycle in the runtime's structured log.
import std.runtime.*

foreign func getchar(): Int32

class PhaseEntryPayload {
    let id: Int64

    init(id: Int64) {
        this.id = id
    }
}

main(): Int64 {
    let slots: Int64 = 800000
    var live = Array<?PhaseEntryPayload>(slots, {_ => None})
    var checksum: Int64 = 0

    // Each slot is written twice.  The first object demonstrably escapes
    // through the product write barrier before becoming garbage; the second
    // stays rooted.  Together they cross the 32 MiB young threshold in
    // ordinary small regions without per-object array backing stores.
    for (i in 0..slots) {
        live[i] = PhaseEntryPayload(i)
        live[i] = PhaseEntryPayload(i + slots)
    }

    // The runner releases stdin only after observing a completed minor cycle
    // from this process. A fixed delay can instead cancel the driver at exit.
    // The foreign call leaves the managed thread available for GC handshakes.
    if (unsafe { getchar() } != 10) {
        println("PHASE_ENTRY_MINOR_FAIL completion_ack")
        return 1
    }

    if (let Some(first) <- live[0]) {
        checksum = checksum + first.id
    }
    if (let Some(last) <- live[slots - 1]) {
        checksum = checksum + last.id
    }
    println("PHASE_ENTRY_MINOR_OK checksum=${checksum}")
    return 0
}
