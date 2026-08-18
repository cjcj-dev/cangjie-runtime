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
    // After a young that collects junk, heapThreshold collapses to ~1.25*live
    // (~8MiB). The next 4.5MiB of alloc hits HEU, which full-scans B and
    // makes the promoted remset edge non-load-bearing (arm P 8/8 golden).
    //
    // 16e6 Int64 ≈ 128MiB old ballast after the first HEU keeps
    // threshold − live ≈ 32MiB so the young line (RegionManager.cpp:1493)
    // fires first. r11 packing otherwise unchanged. Golden = walk(B) =
    // sum(1..8192) = 33558528.
    let junkSlots: Int64 = 256
    var junk = Array<PromoLeaf>(junkSlots, {i => PromoLeaf(-(i + 1))})
    var cursor: Int64 = 0
    var acc: Int64 = 0

    for (round in 0..400) {
        for (i in 0..3200) {
            let n = PromoLeaf(round * 10000 + i)
            junk[cursor % junkSlots] = n
            acc = acc + n.id
            cursor = cursor + 1
        }
        acc = acc + junk[(cursor + round) % junkSlots].id
    }
    println("PROMOTED_EDGE_WARMUP acc=${acc}")

    let ballast = Array<Int64>(16000000, {i => i})
    acc = acc + ballast[0] + ballast[15999999]
    println("PROMOTED_EDGE_BALLAST acc=${acc}")

    var pad = Array<PromoLeaf>(8192, {i => PromoLeaf(-10000 - i)})
    acc = acc + pad[0].id
    pad = Array<PromoLeaf>(1, {_ => PromoLeaf(0)})

    let nHolders: Int64 = 8192
    var holders = Array<PromoHolder>(nHolders, {i => PromoHolder(i)})
    for (i in 0..nHolders) {
        holders[i].kid = None
        acc = acc + holders[i].id
    }

    pad = Array<PromoLeaf>(8192, {i => PromoLeaf(-20000 - i)})
    acc = acc + pad[0].id
    pad = Array<PromoLeaf>(1, {_ => PromoLeaf(0)})

    for (round in 0..400) {
        for (i in 0..3200) {
            let n = PromoLeaf(20000000 + round * 10000 + i)
            junk[cursor % junkSlots] = n
            acc = acc + n.id
            cursor = cursor + 1
        }
        acc = acc + holders[round % nHolders].id
        acc = acc + junk[(cursor + round) % junkSlots].id
    }
    println("PROMOTED_EDGE_AGED acc=${acc}")

    pad = Array<PromoLeaf>(8192, {i => PromoLeaf(-30000 - i)})
    acc = acc + pad[0].id
    pad = Array<PromoLeaf>(1, {_ => PromoLeaf(0)})

    for (i in 0..nHolders) {
        let leaf = PromoLeaf(i + 1)
        holders[i].kid = leaf
        acc = acc + leaf.id
    }
    println("PROMOTED_EDGE_LINKED acc=${acc}")

    pad = Array<PromoLeaf>(8192, {i => PromoLeaf(-40000 - i)})
    acc = acc + pad[0].id
    pad = Array<PromoLeaf>(1, {_ => PromoLeaf(0)})

    for (wave in 0..3) {
        for (round in 0..400) {
            for (i in 0..3200) {
                let n = PromoLeaf(80000000 + wave * 10000000 + round * 10000 + i)
                junk[cursor % junkSlots] = n
                acc = acc + n.id
                cursor = cursor + 1
            }
            let h = holders[round % nHolders]
            acc = acc + h.id
            if (let Some(k) <- h.kid) {
                acc = acc + k.id
            }
            acc = acc + junk[(cursor + round) % junkSlots].id
            acc = acc + ballast[wave * 400 + round]
        }
        println("PROMOTED_EDGE_DRIVE wave=${wave} acc=${acc}")
    }

    var walk: Int64 = 0
    var missing: Int64 = 0
    for (i in 0..nHolders) {
        if (let Some(k) <- holders[i].kid) {
            walk = walk + k.id
        } else {
            missing = missing + 1
        }
    }
    acc = acc + walk + junk[0].id + pad[0].id + ballast[100]
    println("PROMOTED_EDGE_OK checksum=${walk} missing=${missing} holders=${nHolders} acc=${acc}")
    return 0
}
