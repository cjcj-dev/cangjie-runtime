// Real compiler allocation and runtime TLAB boundary coverage.
import std.env.getCommandLine

foreign func TLABBefore(): Unit
foreign func TLABAfter(expected: Int64, actual: Int64): Int64

class InlineTLABPayload {
    let value: Int64
    init(value: Int64) { this.value = value }
}
var inlineTLABSink: ?InlineTLABPayload = None

main(): Int64 {
    let mode = getCommandLine()[1]
    let wanted: Int64 = if (mode == "fast") { 1 } else { 2 }
    // One bounded refill, not a stress workload. Escaping via a global keeps
    // allocation in the compiled product path.
    for (i in 0..2048) {
        unsafe { TLABBefore() }
        let object = InlineTLABPayload(i)
        inlineTLABSink = object
        let result = unsafe { TLABAfter(i, object.value) }
        if (result < 0) { return 1 }
        if (result == wanted) {
            println("TLAB_MANAGED_TARGET mode=${mode} iteration=${i} result=${result}")
            return 0
        }
    }
    return 2
}
