/* Direct installed-stdlib regression for the complete R16 correction chain. */
import std.collection.concurrent.*
import std.runtime.*

type Payload = (Int64, Array<String>, String, Array<Int64>)

class BucketKey <: Hashable & Equatable<BucketKey> {
    let id: Int64
    let hash: Int64
    init(id: Int64, hash: Int64) { this.id = id; this.hash = hash }
    public func hashCode(): Int64 { hash }
    public operator func ==(right: BucketKey): Bool { id == right.id }
    public operator func !=(right: BucketKey): Bool { id != right.id }
}

class CallbackCounts {
    var predicates: Int64 = 0
    var evals: Int64 = 0
}

func payload(id: Int64): Payload {
    (id, Array<String>(8, repeat: "payload-${id}"), "label-${id}", Array<Int64>(8, repeat: id))
}

func valid(v: Payload, id: Int64): Bool {
    v[0] == id && v[1][7] == "payload-${id}" && v[2] == "label-${id}" && v[3][7] == id
}

func require(ok: Bool, name: String): Unit {
    if (!ok) { throw Exception("CHM_ASSERT ${name}") }
}

main(args: Array<String>): Int64 {
    if (args.size > 0 && args[0] == "control") {
        let map = ConcurrentHashMap<BucketKey, Payload>()
        require(map.addIfAbsent(BucketKey(1, 7), payload(1)).isNone(), "control-insert")
        require(valid(map.get(BucketKey(1, 7)).getOrThrow(), 1), "control-get")
        println("CHM_INDEPENDENT_PASS")
        return 0
    }

    let emptyBucket = ConcurrentHashMap<BucketKey, Payload>()
    emptyBucket.add(BucketKey(1, 7), payload(1))
    require(valid(emptyBucket.remove(BucketKey(1, 7)).getOrThrow(), 1), "drop-without-previous")
    require(emptyBucket.isEmpty() && emptyBucket.get(BucketKey(1, 7)).isNone(), "empty-bucket-dropped")

    // These hashes share the old bucket and split after resizing. Removing the
    // only entry in a migrated bucket must not expose the stale previous table.
    let split = ConcurrentHashMap<BucketKey, Payload>(16)
    for (i in 0..3) { split.add(BucketKey(i, i * 16), payload(i)) }
    println("CHM_REMOVE_TARGET_REACHED")
    require(valid(split.remove(BucketKey(0, 0)).getOrThrow(), 0), "split-old-value")
    require(split.get(BucketKey(0, 0)).isNone(), "no-stale-fallback")
    for (i in 1..3) { require(valid(split.get(BucketKey(i, i * 16)).getOrThrow(), i), "split-survivor") }

    // Copy-on-write collision lists: grow, overwrite, two-segment remove and
    // retained iterator entries must preserve every embedded reference.
    let map = ConcurrentHashMap<BucketKey, Payload>()
    for (i in 0..9) { map.add(BucketKey(i, 7), payload(i)) }
    let snapshot = map.iterator()
    require(valid(map.add(BucketKey(3, 7), payload(103)).getOrThrow(), 3), "overwrite-old")
    let counts = CallbackCounts()
    require(map.remove(BucketKey(99, 7), { _ => counts.predicates++; true }).isNone(), "missing-remove")
    require(counts.predicates == 0, "missing-predicate-zero")
    require(map.remove(BucketKey(4, 7), { _ => counts.predicates++; false }).isNone(), "reject-remove")
    require(counts.predicates == 1, "reject-predicate-once")
    require(valid(map.replace(BucketKey(5, 7), { v => counts.predicates++; valid(v, 5) },
        { _ => counts.evals++; payload(105) }).getOrThrow(), 5), "replace-old")
    require(counts.predicates == 2 && counts.evals == 1, "replace-callbacks-once")
    require(map.replace(BucketKey(6, 7), { _ => counts.predicates++; false },
        { _ => counts.evals++; payload(106) }).isNone(), "reject-replace")
    require(counts.predicates == 3 && counts.evals == 1, "reject-eval-zero")
    require(map.replace(BucketKey(99, 7), { _ => counts.predicates++; true },
        { _ => counts.evals++; payload(199) }).isNone(), "missing-replace")
    require(counts.predicates == 3 && counts.evals == 1, "missing-callbacks-zero")
    require(valid(map.remove(BucketKey(4, 7), { _ => counts.predicates++; true }).getOrThrow(), 4), "middle-remove")
    require(counts.predicates == 4, "remove-predicate-once")
    require(valid(map.remove(BucketKey(0, 7)).getOrThrow(), 0), "first-remove")
    require(valid(map.remove(BucketKey(8, 7)).getOrThrow(), 8), "last-remove")
    map.entryView(BucketKey(20, 7), { view => view.value = payload(20) })
    map.entryView(BucketKey(20, 7), { view => view.value = None })
    require(map.addIfAbsent(BucketKey(3, 7), payload(999)).isSome(), "frozen-existing")
    require(map.addIfAbsent(BucketKey(40, 7), payload(40)).isNone(), "frozen-absent")

    let beforeGC = getGCCount()
    gc(heavy: true)
    require(getGCCount() > beforeGC, "gc-completed")
    var snapCount = 0
    for ((key, value) in snapshot) {
        require(valid(value, key.id), "retained-bucket-snapshot")
        snapCount++
    }
    require(snapCount == 9, "snapshot-size")
    require(valid(map.get(BucketKey(3, 7)).getOrThrow(), 103), "overwrite-reference")
    require(valid(map.get(BucketKey(5, 7)).getOrThrow(), 105), "replace-reference")
    require(map.get(BucketKey(4, 7)).isNone() && map.size == 7, "remaining-size")

    // Two small writers force real migration/freeze retries and reference copies.
    let concurrent = ConcurrentHashMap<BucketKey, Payload>(16)
    let a = spawn { =>
        for (i in 0..48) { concurrent.add(BucketKey(i, (i % 8) * 16), payload(i)) }
    }
    let b = spawn { =>
        for (i in 48..96) { concurrent.add(BucketKey(i, (i % 8) * 16), payload(i)) }
    }
    a.get(); b.get()
    gc(heavy: true)
    require(concurrent.size == 96, "concurrent-size")
    for (i in 0..96) {
        require(valid(concurrent.get(BucketKey(i, (i % 8) * 16)).getOrThrow(), i), "migrated-reference")
    }
    println("CHM_BATCH_PASS predicates=${counts.predicates} evals=${counts.evals} migrated=96 gc=${getGCCount() - beforeGC}")
    return 0
}
