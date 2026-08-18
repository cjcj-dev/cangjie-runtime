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

func plantJunk(junk: Array<PromoLeaf>, cursor: Int64, id: Int64): Int64 {
    let n = PromoLeaf(id)
    junk[cursor % Int64(junk.size)] = n
    return n.id
}

func driveJunk(junk: Array<PromoLeaf>, startId: Int64, count: Int64, startCursor: Int64): (Int64, Int64) {
    var checksum: Int64 = 0
    var cursor = startCursor
    for (i in 0..count) {
        checksum = checksum + plantJunk(junk, cursor, startId + i)
        cursor = cursor + 1
    }
    checksum = checksum + junk[0].id
    checksum = checksum + junk[Int64(junk.size) - 1].id
    return (checksum, cursor)
}

main(): Int64 {
    let junkSlots: Int64 = 4096
    var junk = Array<PromoLeaf>(junkSlots, {i => PromoLeaf(-1000000 - i)})
    var checksum: Int64 = 0
    var cursor: Int64 = 0
    var wave: (Int64, Int64)

    // 1.6e6 * ~24B ≈ 38MiB young allocated → crosses the 32MiB young line
    // (RegionManager.cpp:1493) before cohort A exists.
    wave = driveJunk(junk, 0, 1600000, cursor)
    checksum = checksum + wave[0]
    cursor = wave[1]
    println("PROMOTED_EDGE_WARMUP checksum=${checksum}")

    let nHolders: Int64 = 8192
    var holders = Array<PromoHolder>(nHolders, {i => PromoHolder(i)})
    for (i in 0..nHolders) {
        holders[i].kid = None
        checksum = checksum + holders[i].id
    }
    wave = driveJunk(junk, 3000000, 1600000, cursor)
    checksum = checksum + wave[0]
    cursor = wave[1]
    checksum = checksum + holders[0].id
    println("PROMOTED_EDGE_AGED checksum=${checksum}")

    // Cohort B: fresh age=0, reachable only through A. Written while A is still
    // young so the barrier skips remset (Barrier.cpp:1330).
    for (i in 0..nHolders) {
        let leaf = PromoLeaf(i + 1)
        holders[i].kid = leaf
        checksum = checksum + leaf.id
    }
    println("PROMOTED_EDGE_LINKED checksum=${checksum}")

    for (round in 0..4) {
        wave = driveJunk(junk, 8000000 + round * 1600000, 1600000, cursor)
        checksum = checksum + wave[0]
        cursor = wave[1]
        let h = holders[round]
        checksum = checksum + h.id
        if (let Some(k) <- h.kid) {
            checksum = checksum + k.id
        }
    }

    var walk: Int64 = 0
    for (i in 0..nHolders) {
        if (let Some(k) <- holders[i].kid) {
            walk = walk + k.id
        }
    }
    checksum = checksum + walk
    checksum = checksum + junk[1].id
    println("PROMOTED_EDGE_OK checksum=${checksum} walk=${walk} holders=${nHolders}")
    return 0
}
