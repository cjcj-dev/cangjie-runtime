class PromoLeaf {
    public var id: Int64

    public init(id: Int64) {
        this.id = id
    }
}

class PromoHolder {
    public var id: Int64
    public var kid: ?PromoLeaf

    public init(id: Int64) {
        this.id = id
        this.kid = None
    }
}

main(): Int64 {
    // Rotating junk forces heap allocation (escape). 256 slots stay under the
    // 32KiB large-object line so they enter the minor CSet.
    let junkSlots: Int64 = 256
    var junk = Array<PromoLeaf>(junkSlots, {i => PromoLeaf(-1000000 - i)})
    var junkCursor: Int64 = 0
    var checksum: Int64 = 0

    // ~2.2e6 * 24B ≈ 53MiB young allocated → at least one 32MiB young cycle
    // before cohort A exists (RegionManager.cpp:1493).
    for (i in 0..2200000) {
        let n = PromoLeaf(i)
        junk[junkCursor % junkSlots] = n
        checksum = checksum + n.id
        junkCursor = junkCursor + 1
    }
    println("PROMOTED_EDGE_WARMUP checksum=${checksum}")

    // Cohort A: 8192 small holders packed into a few 128KiB regions.
    let nHolders: Int64 = 8192
    var holders = Array<PromoHolder>(nHolders, {i => PromoHolder(i)})
    for (i in 0..nHolders) {
        holders[i].kid = None
        checksum = checksum + holders[i].id
    }
    for (i in 0..2200000) {
        let n = PromoLeaf(3000000 + i)
        junk[junkCursor % junkSlots] = n
        checksum = checksum + n.id
        junkCursor = junkCursor + 1
        if (i % 256 == 0) {
            checksum = checksum + holders[i % nHolders].id
        }
    }
    println("PROMOTED_EDGE_AGED checksum=${checksum}")

    // Cohort B: fresh age=0, reachable only through A. Write while A is still
    // young so the barrier skips remset (Barrier.cpp:1330).
    for (i in 0..nHolders) {
        let leaf = PromoLeaf(i + 1)
        holders[i].kid = leaf
        checksum = checksum + leaf.id
    }
    println("PROMOTED_EDGE_LINKED checksum=${checksum}")

    for (i in 0..6600000) {
        let n = PromoLeaf(8000000 + i)
        junk[junkCursor % junkSlots] = n
        checksum = checksum + n.id
        junkCursor = junkCursor + 1
        if (i % 256 == 0) {
            let h = holders[i % nHolders]
            checksum = checksum + h.id
            if (let Some(k) <- h.kid) {
                checksum = checksum + k.id
            }
        }
    }

    var walk: Int64 = 0
    for (i in 0..nHolders) {
        if (let Some(k) <- holders[i].kid) {
            walk = walk + k.id
        }
    }
    checksum = checksum + walk
    checksum = checksum + junk[0].id
    println("PROMOTED_EDGE_OK checksum=${checksum} walk=${walk} holders=${nHolders}")
    return 0
}
