// End-to-end major forwarding-carrier entry regression.  The bounded escaped
// object wave reaches WCollector::PostTrace through an explicit heavy collect.
// The bounded wave stays well below the product's 32 MiB automatic-minor
// waterline, so the only young cycle is the major driver's owned prelude.
import std.runtime.*

class MajorEntryPayload {
    let id: Int64

    init(id: Int64) {
        this.id = id
    }
}

main(): Int64 {
    let slots: Int64 = 16384
    var live = Array<?MajorEntryPayload>(slots, {_ => None})
    var checksum: Int64 = 0

    // Two escaped generations per slot create ordinary pages while remaining
    // far below the automatic-minor waterline. The explicit heavy entry below
    // is the sole request source.
    for (i in 0..slots) {
        live[i] = MajorEntryPayload(i)
        live[i] = MajorEntryPayload(i + slots)
    }

    gc(heavy: true)
    if (let Some(first) <- live[0]) {
        checksum = checksum + first.id
    }
    if (let Some(last) <- live[slots - 1]) {
        checksum = checksum + last.id
    }
    println("PHASE_ENTRY_MAJOR_OK checksum=${checksum}")
    return 0
}
