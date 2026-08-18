class PromoLeaf {
    public var id: Int64

    public init(id: Int64) {
        this.id = id
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
    // 200000 * 16B ≈ 3.2MiB per burst. 8 bursts ≈ 25MiB → first HEU major
    // (heapThreshold starts at 20MiB) while no live cohort exists.
    // After that major the threshold rises so the 32MiB young line can fire.
    let burst: Int64 = 200000
    let nHolders: Int64 = 32768
    var checksum: Int64 = 0

    for (round in 0..8) {
        checksum = checksum + absorbGarbage(burst, round * 1000000)
    }
    println("PROMOTED_EDGE_WARMUP checksum=${checksum}")

    // Cohort A: one dense Array in a large young region. Initial dummy leaves die;
    // the array stays rooted. Next minor ages the array's region (eden → survivor).
    var holders = Array<PromoLeaf>(nHolders, {i => PromoLeaf(-(i + 1))})
    checksum = checksum + nHolders
    for (round in 0..12) {
        checksum = checksum + absorbGarbage(burst, 10000000 + round * 1000000)
        checksum = checksum + holders[round % nHolders].id
    }
    println("PROMOTED_EDGE_AGED checksum=${checksum}")

    // Cohort B: fresh age=0 leaves, reachable only through A. Written while A is
    // still young so the barrier skips remset (Barrier.cpp:1330 holder-young).
    for (i in 0..nHolders) {
        let leaf = PromoLeaf(i + 1)
        holders[i] = leaf
        checksum = checksum + leaf.id
    }
    println("PROMOTED_EDGE_LINKED checksum=${checksum}")

    // Drive ≥3 minors. Next minor flip-promotes A (age≥threshold) while B stays
    // young; later minors mark B only via the promoted remset edge.
    for (round in 0..40) {
        checksum = checksum + absorbGarbage(burst, 80000000 + round * 1000000)
        checksum = checksum + holders[round % nHolders].id
    }

    var walk: Int64 = 0
    for (i in 0..nHolders) {
        walk = walk + holders[i].id
    }
    checksum = checksum + walk
    println("PROMOTED_EDGE_OK checksum=${checksum} walk=${walk} holders=${nHolders}")
    return 0
}
