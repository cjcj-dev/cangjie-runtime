// Language-level entry for the managed half of segmented object-array roots.
// The test product configuration requests one GC at the first 64 KiB yield;
// this compiled frame supplies real managed stack metadata to the collector.
class ManagedSegmentedPayload {
    let value: Int64

    init(value: Int64) {
        this.value = value
    }
}

main(): Int64 {
    let seed = ManagedSegmentedPayload(37)
    let array = Array<ManagedSegmentedPayload>(20000, {_ => seed})
    let checksum = array[0].value
    println("SEGMENTED_ARRAY_MANAGED_FIXTURE_OK checksum=${checksum}")
    if (checksum != 37) {
        return 1
    }
    return 0
}
