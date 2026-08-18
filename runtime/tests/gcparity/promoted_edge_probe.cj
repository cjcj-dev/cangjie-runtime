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
    // cjEnableGC must be unset at compile time (cjEnableGC=0 bakes RequestGC off).
    //
    // Packing: a 128KiB TL region that mixes live B with later junk is selected
    // for copy and B lands in an old dest (RegionManager.cpp:2709 dest is old).
    // Pad 8192 leaves (~128KiB) and drop them before/after A and B so each
    // cohort owns its own dense regions (ExemptFromRegions + stay-young).
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

    pad = Array<PromoLeaf>(8192, {i => PromoLeaf(-40000 - i)})
    checksum = checksum + pad[0].id
    pad = Array<PromoLeaf>(1, {_ => PromoLeaf(0)})

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
        }
        println("PROMOTED_EDGE_DRIVE wave=${wave} checksum=${checksum}")
    }

    var walk: Int64 = 0
    for (i in 0..nHolders) {
        if (let Some(k) <- holders[i].kid) {
            walk = walk + k.id
        }
    }
    checksum = checksum + walk + junk[0].id + pad[0].id
    println("PROMOTED_EDGE_OK checksum=${checksum} walk=${walk} holders=${nHolders} cursor=${cursor}")
    return 0
}
