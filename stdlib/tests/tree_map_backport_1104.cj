/* Direct installed-stdlib regression for R17 search/deletion/rebalancing. */
import std.collection.*
import std.runtime.*

func require(ok: Bool, name: String): Unit {
    if (!ok) { throw Exception("TREE_ASSERT ${name}") }
}

func checkOrder(map: TreeMap<Int64, Array<String>>, present: Array<Bool>): Unit {
    var expected = 0
    var count = 0
    for ((key, value) in map) {
        while (expected < present.size && !present[expected]) { expected++ }
        require(expected < present.size && key == expected, "iteration-order")
        require(value[0] == "value-${key}", "iteration-reference")
        expected++
        count++
    }
    while (expected < present.size && !present[expected]) { expected++ }
    require(expected == present.size && count == map.size, "iteration-size")
}

main(args: Array<String>): Int64 {
    if (args.size > 0 && args[0] == "control") {
        let control = TreeMap<Int64, Array<String>>()
        control.add(1, ["value-1"])
        require(control.get(1).getOrThrow()[0] == "value-1", "control-get")
        println("TREE_INDEPENDENT_PASS")
        return 0
    }
    // Degree six: 144 sorted inserts produce multiple internal levels. Opposite
    // insertion/deletion orders and a coprime permutation exercise both siblings.
    for (mode in 0..3) {
        let map = TreeMap<Int64, Array<String>>()
        let present = Array<Bool>(144, repeat: true)
        for (i in 0..144) {
            let key = if (mode == 1) { 143 - i } else { i }
            require(map.add(key, ["value-${key}"]).isNone(), "initial-insert")
        }
        require(map.get(-1).isNone() && map.get(145).isNone(), "deep-missing-search")
        require(map.remove(145).isNone(), "missing-remove")
        require(map.add(71, ["replacement"]).getOrThrow()[0] == "value-71", "overwrite-old")
        require(map.add(71, ["value-71"]).getOrThrow()[0] == "replacement", "restore-old")
        let asMap: Map<Int64, Array<String>> = map
        require(asMap.addIfAbsent(71, ["ignored"]).getOrThrow()[0] == "value-71", "map-static-dispatch")
        checkOrder(map, present)
        let beforeGC = getGCCount()
        gc(heavy: true)
        require(getGCCount() > beforeGC, "gc-completed")
        println("TREE_REMOVE_TARGET_REACHED mode=${mode}")
        for (i in 0..144) {
            let key = if (mode == 0) { i } else if (mode == 1) { 143 - i } else { (i * 37) % 144 }
            require(map.remove(key).getOrThrow()[0] == "value-${key}", "delete-old-value")
            present[key] = false
            require(map.size == 143 - i && map.get(key).isNone(), "delete-size-and-missing")
            checkOrder(map, present)
        }
        require(map.isEmpty() && map.first.isNone() && map.last.isNone(), "root-shrink-empty")
        require(map.forward(1).next().isNone() && map.backward(1).next().isNone(), "empty-range-baseline")
        map.add(1, ["one"])
        map.add(2, ["two"])
        require(map.removeFirst().getOrThrow()[0] == 1 && map.removeLast().getOrThrow()[0] == 2, "root-reuse")
    }
    let versions = TreeMap<Int64, Int64>()
    versions.add(1, 1)
    versions.add(2, 2)
    let iterator = versions.iterator()
    versions.remove(1)
    var rejected = false
    try { iterator.next() } catch (e: ConcurrentModificationException) { rejected = true }
    require(rejected, "iterator-invalidated")
    println("TREE_BATCH_PASS modes=3 deleted=432")
    return 0
}
