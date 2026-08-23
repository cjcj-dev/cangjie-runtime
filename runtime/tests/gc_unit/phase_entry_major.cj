// End-to-end major forwarding-carrier entry regression.  The bounded escaped
// object wave reaches WCollector::PostTrace; explicit heavy collection remains
// as a fallback, and the harness disables standalone automatic minors.
import std.runtime.*

class MajorEntryPayload {
    let id: Int64

    init(id: Int64) {
        this.id = id
    }
}

main(): Int64 {
    let slots: Int64 = 200000
    var live = Array<?MajorEntryPayload>(slots, {_ => None})
    var checksum: Int64 = 0

    // Two escaped generations per slot create selected ordinary pages while
    // remaining bounded; either the deterministic heuristic entry or the
    // explicit user entry reaches PostTrace.
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
