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
    let nHolders: Int64 = 16384
    let garbageBurst: Int64 = 8192
    var holders = Array<PromoHolder>(nHolders, {i => PromoHolder(i)})
    var checksum: Int64 = 0

    // Age cohort A across at least one minor (age 0 → 1, threshold 1 keeps eden).
    // Dummy leaves die; holders stay rooted so their regions tenure on the next minor.
    for (i in 0..nHolders) {
        holders[i].kid = PromoLeaf(-(i + 1))
        checksum = checksum + holders[i].id
    }
    for (round in 0..80) {
        checksum = checksum + absorbGarbage(garbageBurst, round * 100000)
        checksum = checksum + holders[round % nHolders].id
    }
    println("PROMOTED_EDGE_AGED checksum=${checksum}")

    // Cohort B: fresh age=0 leaves, reachable only through A. Write while A is still young
    // so the barrier skips remset (young→young). After the next minor A flip-promotes and
    // B stays young — the remset must carry the promoted edge.
    for (i in 0..nHolders) {
        let leaf = PromoLeaf(i + 1)
        holders[i].kid = leaf
        checksum = checksum + leaf.id
    }
    println("PROMOTED_EDGE_LINKED checksum=${checksum}")

    // Drive ≥3 more minors so B is marked only via the promoted remset edge.
    for (round in 0..240) {
        checksum = checksum + absorbGarbage(garbageBurst, 80000000 + round * 100000)
        let h = holders[round % nHolders]
        checksum = checksum + h.id
        if (let Some(k) <- h.kid) {
            checksum = checksum + k.id
        }
    }

    var walk: Int64 = 0
    for (i in 0..nHolders) {
        if (let Some(k) <- holders[i].kid) {
            walk = walk + k.id
        }
    }
    checksum = checksum + walk
    println("PROMOTED_EDGE_OK checksum=${checksum} walk=${walk} holders=${nHolders}")
    return 0
}
