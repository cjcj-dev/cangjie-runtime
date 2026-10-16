// Managed compiler allocation and checksum coverage. The runner's full/young
// initialization-window checks use the separately built native external fixture;
// this program does not claim a managed-stack GC initialization window.
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
