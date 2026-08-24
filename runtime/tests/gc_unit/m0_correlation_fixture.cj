// Product-allocation integration fixture for the opaque M0 correlation ABI.

class CorrelationFixtureObject {
    let value: UInt64

    init(value: UInt64) {
        this.value = value
    }
}

foreign func MRT_M0CorrTagNextAllocation(externalKey: UInt64): Unit
foreign func MRT_M0CorrObserve(consumedObject: Object, externalKey: UInt64, consumerToken: UInt64,
    payload0: UInt64, payload1: UInt64): Unit
foreign func MRT_M0CorrRelease(externalKey: UInt64): Unit
foreign func MRT_M0CorrSelectKeepLive(externalKey: UInt64): Unit
foreign func MRT_M0CorrIsSelected(externalKey: UInt64): UInt8

main(): Int64 {
    let externalKey = UInt64(73014444033)
    unsafe { MRT_M0CorrSelectKeepLive(UInt64(0)) }
    unsafe { MRT_M0CorrTagNextAllocation(externalKey) }
    let item = CorrelationFixtureObject(UInt64(37))
    unsafe {
        MRT_M0CorrObserve(item, externalKey, UInt64(91), item.value, UInt64(5))
    }
    let selected = unsafe { MRT_M0CorrIsSelected(externalKey) }
    unsafe { MRT_M0CorrRelease(externalKey) }
    println("M0_CORRELATION_FIXTURE_OK selected=${selected}")
    return 0
}
