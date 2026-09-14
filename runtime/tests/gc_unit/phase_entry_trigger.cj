// End-to-end minor driver entry regression. This fixture requests a minor;
// it does not test the director's automatic allocation-trigger policy.
import std.runtime.*
import std.env.getCommandLine
import std.fs.exists

foreign func PhaseEntryRequestMinor(): Unit

class PhaseEntryPayload {
    let id: Int64

    init(id: Int64) {
        this.id = id
    }
}

main(): Int64 {
    let completion = getCommandLine()[1]
    let slots: Int64 = 16384
    var live = Array<?PhaseEntryPayload>(slots, {_ => None})
    var checksum: Int64 = 0

    // Each slot is written twice.  The first object demonstrably escapes
    // through the product write barrier before becoming garbage; the second
    // stays rooted. The bounded wave stays below director warmup pressure;
    // the fixture bridge below is the sole request source.
    for (i in 0..slots) {
        live[i] = PhaseEntryPayload(i)
        live[i] = PhaseEntryPayload(i + slots)
    }

    unsafe { PhaseEntryRequestMinor() }

    // The runner creates this private receipt only after reading a completed
    // minor cycle from this process. Polling yields to managed GC handshakes;
    // a fixed delay cannot prove completion. The runner bounds the whole run.
    while (!exists(completion)) {
        sleep(10 * Duration.millisecond)
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
