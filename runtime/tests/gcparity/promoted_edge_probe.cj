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
    // Compile with cjEnableGC unset (cjEnableGC=0 bakes RequestGC off).
    //
    // r11 wrote B AFTER the first young had already flip-promoted A, so
    // holders[i].kid = leaf was old→young and the mutator barrier recorded
    // remset (Barrier.cpp:1354). Arm P cut only promote/discharge Record,
    // not that barrier — 8/8 still golden.
    //
    // This shape writes B while A is still young (no GC between A and B),
    // then drives young-only cycles:
    //   young 1: A,B age 0 → stay-young (threshold=1)
    //   young 2: A age 1 → residual flip-promote; B age 0 stays; Record A→B
    //   young 3+: B must be marked via remset (FYS=false). 24MiB ballast
    //   keeps heapThreshold above the 32MiB young line so HEU does not
    //   full-scan-rescue B.
    let junkSlots: Int64 = 256
    var junk = Array<PromoLeaf>(junkSlots, {i => PromoLeaf(-(i + 1))})
    var cursor: Int64 = 0
    var checksum: Int64 = 0

    for (round in 0..400) {
        for (i in 0..3200) {
            let n = PromoLeaf(round * 10000 + i)
            junk[cursor % junkSlots] = n
            checksum = checksum + n.id
            cursor = cursor + 1
        }
        checksum = checksum + junk[(cursor + round) % junkSlots].id
    }
    println("PROMOTED_EDGE_WARMUP checksum=${checksum}")

    let ballast = Array<Int64>(3000000, {i => i})
    checksum = checksum + ballast[0] + ballast[2999999]
    println("PROMOTED_EDGE_BALLAST checksum=${checksum}")

    var pad = Array<PromoLeaf>(8192, {i => PromoLeaf(-10000 - i)})
    checksum = checksum + pad[0].id
    pad = Array<PromoLeaf>(1, {_ => PromoLeaf(0)})

    let nHolders: Int64 = 8192
    var holders = Array<PromoHolder>(nHolders, {i => PromoHolder(i)})
    for (i in 0..nHolders) {
        let leaf = PromoLeaf(i + 1)
        holders[i].kid = leaf
        checksum = checksum + holders[i].id + leaf.id
    }
    println("PROMOTED_EDGE_LINKED checksum=${checksum}")

    pad = Array<PromoLeaf>(8192, {i => PromoLeaf(-20000 - i)})
    checksum = checksum + pad[0].id
    pad = Array<PromoLeaf>(1, {_ => PromoLeaf(0)})

    for (wave in 0..4) {
        for (round in 0..400) {
            for (i in 0..3200) {
                let n = PromoLeaf(20000000 + wave * 10000000 + round * 10000 + i)
                junk[cursor % junkSlots] = n
                checksum = checksum + n.id
                cursor = cursor + 1
            }
            let h = holders[round % nHolders]
            checksum = checksum + h.id
            if (let Some(k) <- h.kid) {
                checksum = checksum + k.id
            }
            checksum = checksum + junk[(cursor + round) % junkSlots].id
            checksum = checksum + ballast[wave * 400 + round]
        }
        println("PROMOTED_EDGE_DRIVE wave=${wave} checksum=${checksum}")
    }

    var walk: Int64 = 0
    for (i in 0..nHolders) {
        if (let Some(k) <- holders[i].kid) {
            walk = walk + k.id
        }
    }
    checksum = checksum + walk + junk[0].id + ballast[100] + pad[0].id
    println("PROMOTED_EDGE_OK checksum=${checksum} walk=${walk} holders=${nHolders} cursor=${cursor}")
    return 0
}
