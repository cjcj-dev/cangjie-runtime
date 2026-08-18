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
    // r11 (c800251f) H 8/8 walk=33558528, recEdges=4381. P 8/8 same walk
    // because the 400-round drive is 30.7MiB < 32MiB young line, so HEU
    // full-scans B. Bump drive to 600 rounds (≈46MiB) so young fires first
    // (RegionManager.cpp:1493 before HEU). Golden is walk(B)=sum(1..8192)
    // only — junk ids are timing-dependent.
    let junkSlots: Int64 = 256
    let waveRounds: Int64 = 600
    let waveInner: Int64 = 3200
    var junk = Array<PromoLeaf>(junkSlots, {i => PromoLeaf(-(i + 1))})
    var cursor: Int64 = 0

    for (round in 0..waveRounds) {
        for (i in 0..waveInner) {
            junk[cursor % junkSlots] = PromoLeaf(round * 10000 + i)
            cursor = cursor + 1
        }
    }
    println("PROMOTED_EDGE_WARMUP")

    var pad = Array<PromoLeaf>(8192, {i => PromoLeaf(-10000 - i)})
    pad = Array<PromoLeaf>(1, {_ => PromoLeaf(0)})

    let nHolders: Int64 = 8192
    var holders = Array<PromoHolder>(nHolders, {i => PromoHolder(i)})
    for (i in 0..nHolders) {
        holders[i].kid = None
    }

    pad = Array<PromoLeaf>(8192, {i => PromoLeaf(-20000 - i)})
    pad = Array<PromoLeaf>(1, {_ => PromoLeaf(0)})

    for (round in 0..waveRounds) {
        for (i in 0..waveInner) {
            junk[cursor % junkSlots] = PromoLeaf(20000000 + round * 10000 + i)
            cursor = cursor + 1
        }
        let _keepA = holders[round % nHolders].id
    }
    println("PROMOTED_EDGE_AGED")

    pad = Array<PromoLeaf>(8192, {i => PromoLeaf(-30000 - i)})
    pad = Array<PromoLeaf>(1, {_ => PromoLeaf(0)})

    for (i in 0..nHolders) {
        holders[i].kid = PromoLeaf(i + 1)
    }
    println("PROMOTED_EDGE_LINKED")

    pad = Array<PromoLeaf>(8192, {i => PromoLeaf(-40000 - i)})
    pad = Array<PromoLeaf>(1, {_ => PromoLeaf(0)})

    for (wave in 0..3) {
        for (round in 0..waveRounds) {
            for (i in 0..waveInner) {
                junk[cursor % junkSlots] = PromoLeaf(80000000 + wave * 10000000 + round * 10000 + i)
                cursor = cursor + 1
            }
            let h = holders[round % nHolders]
            let _keepH = h.id
            if (let Some(k) <- h.kid) {
                let _keepK = k.id
            }
        }
        println("PROMOTED_EDGE_DRIVE wave=${wave}")
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
    let _keepPad = pad[0].id
    let _keepJunk = junk[0].id
    println("PROMOTED_EDGE_OK checksum=${walk} missing=${missing} holders=${nHolders}")
    return 0
}
