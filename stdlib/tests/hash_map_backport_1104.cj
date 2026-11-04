/* Minimal product-chain regression for HashMap collision and value-slot reuse. */
import std.collection.*

class CollisionKey <: Hashable & Equatable<CollisionKey> {
    let id: Int64

    init(id: Int64) {
        this.id = id
    }

    public func hashCode(): Int64 {
        return 7
    }

    public operator func ==(right: CollisionKey): Bool {
        return this.id == right.id
    }

    public operator func !=(right: CollisionKey): Bool {
        return this.id != right.id
    }
}

main(): Int64 {
    let map = HashMap<CollisionKey, Array<String>>()
    for (i in 0..41) {
        map.add(CollisionKey(i), Array<String>(128, repeat: "payload"))
    }
    if (map.size != 41 || !map.contains(CollisionKey(17))) {
        return 1
    }
    if (map.addIfAbsent(CollisionKey(17), Array<String>(128, repeat: "replacement")).isNone()) {
        return 2
    }
    if (map.remove(CollisionKey(17)).isNone() || map.contains(CollisionKey(17))) {
        return 3
    }
    if (map.addIfAbsent(CollisionKey(100), Array<String>(128, repeat: "reused")).isSome()) {
        return 4
    }

    let asMap: Map<CollisionKey, Array<String>> = map
    if (asMap.addIfAbsent(CollisionKey(100), Array<String>(128, repeat: "ignored")).isNone()) {
        return 5
    }
    if (!asMap.contains(CollisionKey(100))) {
        return 6
    }
    println("HASHMAP_BACKPORT completed=1 size=${map.size}")
    return 0
}
