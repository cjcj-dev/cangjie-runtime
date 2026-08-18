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
    // Same escape shape as survival_dense.cj:12-28 — store every new object
    // into a rooted array so cjc cannot scalar-replace the allocation.
    let junkSlots: Int64 = 65536
    var junk = Array<PromoLeaf>(junkSlots, {i => PromoLeaf(-(i + 1))})
    var cursor: Int64 = 0
    var checksum: Int64 = 0

    // 400 * 3200 = 1.28e6 objects ≈ 30+MiB. Two waves cross the 32MiB young line
    // (RegionManager.cpp:1493) before A is linked to B.
    for (wave in 0..2) {
        for (round in 0..400) {
            for (i in 0..3200) {
                let id = wave * 10000000 + round * 10000 + i
                let n = PromoLeaf(id)
                junk[cursor % junkSlots] = n
                checksum = checksum + n.id
                cursor = cursor + 1
            }
            checksum = checksum + junk[(cursor + round) % junkSlots].id
        }
        println("PROMOTED_EDGE_WAVE wave=${wave} checksum=${checksum}")
    }
    println("PROMOTED_EDGE_WARMUP checksum=${checksum}")

    let nHolders: Int64 = 8192
    var holders = Array<PromoHolder>(nHolders, {i => PromoHolder(i)})
    for (i in 0..nHolders) {
        holders[i].kid = None
        checksum = checksum + holders[i].id
    }

    for (round in 0..400) {
        for (i in 0..3200) {
            let id = 30000000 + round * 10000 + i
            let n = PromoLeaf(id)
            junk[cursor % junkSlots] = n
            checksum = checksum + n.id
            cursor = cursor + 1
        }
        checksum = checksum + holders[round % nHolders].id
        checksum = checksum + junk[(cursor + round) % junkSlots].id
    }
    println("PROMOTED_EDGE_AGED checksum=${checksum}")

    for (i in 0..nHolders) {
        let leaf = PromoLeaf(i + 1)
        holders[i].kid = leaf
        checksum = checksum + leaf.id
    }
    println("PROMOTED_EDGE_LINKED checksum=${checksum}")

    for (wave in 0..3) {
        for (round in 0..400) {
            for (i in 0..3200) {
                let id = 80000000 + wave * 10000000 + round * 10000 + i
                let n = PromoLeaf(id)
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
    checksum = checksum + walk
    checksum = checksum + junk[0].id
    println("PROMOTED_EDGE_OK checksum=${checksum} walk=${walk} holders=${nHolders} cursor=${cursor}")
    return 0
}
