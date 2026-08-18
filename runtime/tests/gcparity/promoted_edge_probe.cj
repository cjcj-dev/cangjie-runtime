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

func fillJunk(junk: Array<PromoLeaf>, startId: Int64): Int64 {
    var checksum: Int64 = 0
    let n = Int64(junk.size)
    for (i in 0..n) {
        let leaf = PromoLeaf(startId + i)
        junk[i] = leaf
        checksum = checksum + leaf.id
    }
    return checksum
}

func churnJunk(junk: Array<PromoLeaf>, startId: Int64, rounds: Int64): Int64 {
    var checksum: Int64 = 0
    let n = Int64(junk.size)
    for (r in 0..rounds) {
        for (i in 0..n) {
            let leaf = PromoLeaf(startId + r * n + i)
            junk[i] = leaf
            checksum = checksum + junk[i].id
        }
        checksum = checksum + junk[r % n].id
    }
    return checksum
}

main(): Int64 {
    // 4096 live junk slots stay well under the 32KiB large-object line.
    // 2048 rounds * 4096 allocs ≈ 8.4e6 objects ≈ 200MiB young allocated
    // so the 32MiB young line (RegionManager.cpp:1493) fires several times.
    let junkSlots: Int64 = 4096
    var junk = Array<PromoLeaf>(junkSlots, {i => PromoLeaf(-1)})
    var checksum: Int64 = 0

    checksum = checksum + fillJunk(junk, 1)
    checksum = checksum + churnJunk(junk, 100000, 256)
    println("PROMOTED_EDGE_WARMUP checksum=${checksum}")

    let nHolders: Int64 = 8192
    var holders = Array<PromoHolder>(nHolders, {i => PromoHolder(i)})
    for (i in 0..nHolders) {
        holders[i].kid = None
        checksum = checksum + holders[i].id
    }
    checksum = checksum + churnJunk(junk, 2000000, 256)
    checksum = checksum + holders[0].id
    println("PROMOTED_EDGE_AGED checksum=${checksum}")

    for (i in 0..nHolders) {
        let leaf = PromoLeaf(i + 1)
        holders[i].kid = leaf
        checksum = checksum + leaf.id
    }
    println("PROMOTED_EDGE_LINKED checksum=${checksum}")

    checksum = checksum + churnJunk(junk, 8000000, 768)
    for (i in 0..nHolders) {
        checksum = checksum + holders[i].id
        if (let Some(k) <- holders[i].kid) {
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
    checksum = checksum + junk[7].id
    println("PROMOTED_EDGE_OK checksum=${checksum} walk=${walk} holders=${nHolders}")
    return 0
}
