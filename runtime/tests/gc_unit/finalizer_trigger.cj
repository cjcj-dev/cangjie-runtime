// End-to-end finalizer regression probe. The object allocation is kept in a
// separate function so every reference is out of scope before the major GC.
import std.runtime.*

class FinalizerTriggerObject {
    let id: Int64

    init(id: Int64) {
        this.id = id
    }

    ~init() {
        println("FINALIZER_CALLED id=${id}")
    }
}

func AllocateFinalizerGarbage(count: Int64): Unit {
    for (id in 0..count) {
        let _ = FinalizerTriggerObject(id)
    }
}

main(): Int64 {
    let allocated: Int64 = 64
    AllocateFinalizerGarbage(allocated)

    // Finalization is asynchronous. Multiple synchronous major collections
    // plus scheduler yields give the finalizer worker time to drain its queue.
    for (_ in 0..2) {
        gc(heavy: true)
        sleep(500 * Duration.millisecond)
    }

    println("FINALIZER_TRIGGER_DONE allocated=${allocated}")
    return 0
}
