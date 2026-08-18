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
    // After the first HEU, keep 24MiB Int64 ballast live so heapThreshold
    // stays above the 32MiB young line (RegionManager.cpp:1493). Then A ages
    // on young-only cycles.
    //
    // Pad around A so A owns dense 128KiB pages (residual flip-promote).
    // Do NOT pad after B: dying junk fills the rest of B's TL region so the
    // NEXT young (B age=1, StayYoungThisCycle false) copies B
    // (RelocationSetSelector.h:14). A's slots still name B's from-address
    // and must be in remset (RecordPromotedCrossGenEdges RegionManager.cpp:359).
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

    for (round in 0..400) {
        for (i in 0..3200) {
            let n = PromoLeaf(10000000 + round * 10000 + i)
            junk[cursor % junkSlots] = n
            checksum = checksum + n.id
            cursor = cursor + 1
        }
        checksum = checksum + junk[(cursor + round) % junkSlots].id
        checksum = checksum + ballast[round]
    }
    println("PROMOTED_EDGE_HEU2 checksum=${checksum}")

    var pad = Array<PromoLeaf>(8192, {i => PromoLeaf(-10000 - i)})
    checksum = checksum + pad[0].id
    pad = Array<PromoLeaf>(1, {_ => PromoLeaf(0)})

    let nHolders: Int64 = 8192
    var holders = Array<PromoHolder>(nHolders, {i => PromoHolder(i)})
    for (i in 0..nHolders) {
        holders[i].kid = None
        checksum = checksum + holders[i].id
    }

    pad = Array<PromoLeaf>(8192, {i => PromoLeaf(-20000 - i)})
    checksum = checksum + pad[0].id
    pad = Array<PromoLeaf>(1, {_ => PromoLeaf(0)})

    for (round in 0..400) {
        for (i in 0..3200) {
            let n = PromoLeaf(20000000 + round * 10000 + i)
            junk[cursor % junkSlots] = n
            checksum = checksum + n.id
            cursor = cursor + 1
        }
        checksum = checksum + holders[round % nHolders].id
        checksum = checksum + junk[(cursor + round) % junkSlots].id
        checksum = checksum + ballast[400 + round]
    }
    println("PROMOTED_EDGE_AGED checksum=${checksum}")

    pad = Array<PromoLeaf>(8192, {i => PromoLeaf(-30000 - i)})
    checksum = checksum + pad[0].id
    pad = Array<PromoLeaf>(1, {_ => PromoLeaf(0)})

    for (i in 0..nHolders) {
        let leaf = PromoLeaf(i + 1)
        holders[i].kid = leaf
        checksum = checksum + leaf.id
    }
    println("PROMOTED_EDGE_LINKED checksum=${checksum}")

    for (wave in 0..3) {
        for (round in 0..400) {
            for (i in 0..3200) {
                let n = PromoLeaf(80000000 + wave * 10000000 + round * 10000 + i)
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
            checksum = checksum + ballast[800 + wave * 400 + round]
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
