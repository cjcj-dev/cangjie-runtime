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

func absorbGarbage(n: Int64, salt: Int64): Int64 {
    var sink: Int64 = 0
    for (i in 0..n) {
        let g = PromoLeaf(salt + i)
        sink = sink + g.id
    }
    return sink
}

main(): Int64 {
    // Keep ~24MiB of primitive payload alive so the first HEU major raises
    // heapThreshold above the 32MiB young line (RegionManager.cpp:1493).
    let ballast = Array<Int64>(3000000, {i => i})
    let burst: Int64 = 250000
    let nHolders: Int64 = 8192
    var checksum: Int64 = ballast[0]

    for (round in 0..16) {
        checksum = checksum + absorbGarbage(burst, round * 1000000)
        checksum = checksum + ballast[round]
    }
    println("PROMOTED_EDGE_WARMUP checksum=${checksum}")

    // Cohort A: small holders (not a large array payload). Sequential alloc packs
    // them into dense 128KiB regions so ExemptFromRegions keeps them in-place
    // and residual flip-promote can fire (RegionManager.cpp:2660 / WCollector.cpp:7075).
    var holders = Array<PromoHolder>(nHolders, {i => PromoHolder(i)})
    for (i in 0..nHolders) {
        holders[i].kid = PromoLeaf(-(i + 1))
        checksum = checksum + holders[i].id
    }
    for (round in 0..16) {
        checksum = checksum + absorbGarbage(burst, 20000000 + round * 1000000)
        checksum = checksum + holders[round % nHolders].id
        checksum = checksum + ballast[round + 16]
    }
    println("PROMOTED_EDGE_AGED checksum=${checksum}")

    // Cohort B: fresh age=0 leaves, reachable only through A. Written while A is
    // still young so the barrier skips remset (Barrier.cpp:1330).
    for (i in 0..nHolders) {
        let leaf = PromoLeaf(i + 1)
        holders[i].kid = leaf
        checksum = checksum + leaf.id
    }
    println("PROMOTED_EDGE_LINKED checksum=${checksum}")

    for (round in 0..48) {
        checksum = checksum + absorbGarbage(burst, 80000000 + round * 1000000)
        let h = holders[round % nHolders]
        checksum = checksum + h.id
        if (let Some(k) <- h.kid) {
            checksum = checksum + k.id
        }
        checksum = checksum + ballast[round + 32]
    }

    var walk: Int64 = 0
    for (i in 0..nHolders) {
        if (let Some(k) <- holders[i].kid) {
            walk = walk + k.id
        }
    }
    checksum = checksum + walk
    checksum = checksum + ballast[100]
    println("PROMOTED_EDGE_OK checksum=${checksum} walk=${walk} holders=${nHolders}")
    return 0
}
