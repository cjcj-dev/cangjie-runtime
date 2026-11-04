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

class SplitKey <: Hashable & Equatable<SplitKey> {
    let id: Int64

    init(id: Int64) {
        this.id = id
    }

    public func hashCode(): Int64 {
        return id * 16
    }

    public operator func ==(right: SplitKey): Bool {
        return this.id == right.id
    }

    public operator func !=(right: SplitKey): Bool {
        return this.id != right.id
    }
}

class ReentrantKey <: Hashable & Equatable<ReentrantKey> {
    let id: Int64
    static var reentrantMap: HashMap<ReentrantKey, Array<String>> = HashMap()
    static var reentrantHead: ReentrantKey = ReentrantKey(1)
    static var reentrantEnabled: Bool = false

    init(id: Int64) {
        this.id = id
    }

    public func hashCode(): Int64 {
        return 7
    }

    public operator func ==(right: ReentrantKey): Bool {
        if (ReentrantKey.reentrantEnabled && this.id == 2 && right.id == 1) {
            ReentrantKey.reentrantEnabled = false
            ReentrantKey.reentrantMap.remove(ReentrantKey.reentrantHead)
        }
        return this.id == right.id
    }

    public operator func !=(right: ReentrantKey): Bool {
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

    // Equality may remove the current chain head. The traversal must continue
    // from the entry snapshot and preserve the old value of the live successor.
    ReentrantKey.reentrantMap = HashMap<ReentrantKey, Array<String>>()
    let reentrantB = ReentrantKey(2)
    let reentrantA = ReentrantKey(1)
    ReentrantKey.reentrantHead = reentrantA
    ReentrantKey.reentrantMap.add(reentrantB, Array<String>(2, repeat: "old"))
    ReentrantKey.reentrantMap.add(reentrantA, Array<String>(2, repeat: "head"))
    ReentrantKey.reentrantEnabled = true
    if (!ReentrantKey.reentrantMap.contains(ReentrantKey(2)) || ReentrantKey.reentrantMap.size != 1) {
        return 7
    }

    ReentrantKey.reentrantMap = HashMap<ReentrantKey, Array<String>>()
    ReentrantKey.reentrantHead = ReentrantKey(1)
    ReentrantKey.reentrantMap.add(reentrantB, Array<String>(2, repeat: "old"))
    ReentrantKey.reentrantMap.add(ReentrantKey.reentrantHead, Array<String>(2, repeat: "head"))
    ReentrantKey.reentrantEnabled = true
    let old = ReentrantKey.reentrantMap.add(ReentrantKey(2), Array<String>(2, repeat: "new"))
    match (old) {
        case Some(v) =>
            if (v[0] != "old" || ReentrantKey.reentrantMap.size != 1) { return 8 }
        case None => return 9
    }

    // Delete and reuse a slot, then force a bucket split during resize.
    let splitMap = HashMap<SplitKey, Int64>()
    splitMap.add(SplitKey(0), 0)
    splitMap.add(SplitKey(1), 1)
    splitMap.remove(SplitKey(0))
    splitMap.add(SplitKey(2), 2)
    splitMap.reserve(32)
    if (!splitMap.contains(SplitKey(1)) || !splitMap.contains(SplitKey(2)) ||
        splitMap.get(SplitKey(2)) != Some(2)) {
        return 10
    }
    println("HASHMAP_BACKPORT completed=1 size=${map.size}")
    return 0
}
