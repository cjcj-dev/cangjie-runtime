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
    // Compile MUST leave cjEnableGC unset (cjEnableGC=0 is baked into the ELF
    // and silently disables RequestGC). Shape:
    //   1) junk until the first HEU major (20MiB)
    //   2) 3e6 Int64 ballast so later HEU threshold sits above the 32MiB young line
    //   3) cohort A (small holders, dense 128KiB regions)
    //   4) one young wave — A ages 0→1 and stays young (threshold=1)
    //   5) write B while A is still young (barrier skips remset, Barrier.cpp:1330)
    //   6) more young waves — A flip-promotes, B stays young, remset must carry B
    let junkSlots: Int64 = 65536
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

    let nHolders: Int64 = 8192
    var holders = Array<PromoHolder>(nHolders, {i => PromoHolder(i)})
    for (i in 0..nHolders) {
        holders[i].kid = None
        checksum = checksum + holders[i].id
    }

    for (round in 0..400) {
        for (i in 0..3200) {
            let n = PromoLeaf(20000000 + round * 10000 + i)
            junk[cursor % junkSlots] = n
            checksum = checksum + n.id
            cursor = cursor + 1
        }
        checksum = checksum + holders[round % nHolders].id
        checksum = checksum + junk[(cursor + round) % junkSlots].id
        checksum = checksum + ballast[round]
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
    checksum = checksum + walk
    checksum = checksum + junk[0].id + ballast[100]
    println("PROMOTED_EDGE_OK checksum=${checksum} walk=${walk} holders=${nHolders} cursor=${cursor}")
    return 0
}
