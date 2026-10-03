import std.collection.*
import std.sync.*

main(): Int64 {
    let values = ArrayList<Int64>()
    let mutex = Mutex()
    mutex.lock()
    values.add(1356)
    mutex.unlock()
    let valid = values.size == 1 && values[0] == 1356
    println("REFERENCE1356_STARTUP_CONTROL collection_lock=${valid}")
    if (valid) { 0 } else { 1 }
}
