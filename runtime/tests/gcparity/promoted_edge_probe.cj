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
    // Compile with cjEnableGC unset. `cjEnableGC=0` is baked into the ELF and
    // disables RequestGC (r8 vs r9).
    //
    // 700*3200 PromoLeaf ≈ 36MiB so each wave crosses the 32MiB young line
    // (RegionManager.cpp:1493) before HEU can fire.
    //
    // Cycle N (after B is linked): A has age 1 → residual flip-promote;
    // B has age 0 → StayYoungThisCycle (TenuringThreshold.h:91) even if the
    // rest of B's 128KiB region is dying junk. Discharge then sees A→B still
    // young (PromotedRegionDomain.cpp:354).
    // Cycle N+1: B age 1, same region now >25% garbage → copy to old dest.
    // A.slot still names the from-address; remset recorded at N must fix it.
    let junkSlots: Int64 = 256
    let waveRounds: Int64 = 700
    let waveInner: Int64 = 3200
    var junk = Array<PromoLeaf>(junkSlots, {i => PromoLeaf(-(i + 1))})
    var cursor: Int64 = 0
    var checksum: Int64 = 0

    for (round in 0..waveRounds) {
        for (i in 0..waveInner) {
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

    let nHolders: Int64 = 4096
    var holders = Array<PromoHolder>(nHolders, {i => PromoHolder(i)})
    for (i in 0..nHolders) {
        holders[i].kid = None
        checksum = checksum + holders[i].id
    }

    pad = Array<PromoLeaf>(8192, {i => PromoLeaf(-20000 - i)})
    checksum = checksum + pad[0].id
    pad = Array<PromoLeaf>(1, {_ => PromoLeaf(0)})

    for (round in 0..waveRounds) {
        for (i in 0..waveInner) {
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

    // Do NOT pad. Fill the rest of B's TL region with dying junk so cycle N+1
    // copies B (kRelocationFragmentationLimitPercent, RelocationSetSelector.h:14).
    for (round in 0..waveRounds) {
        for (i in 0..waveInner) {
            let n = PromoLeaf(40000000 + round * 10000 + i)
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
    println("PROMOTED_EDGE_MIXED checksum=${checksum}")

    for (wave in 0..3) {
        for (round in 0..waveRounds) {
            for (i in 0..waveInner) {
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
