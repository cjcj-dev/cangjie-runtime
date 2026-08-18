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
    // Inner arrays stay under the 32KiB large-object line so they enter the
    // minor CSet (PrepareYoungGarbageCandidates walks recentFull, not recentLarge).
    let chunk: Int64 = 1024
    let nChunks: Int64 = 8
    let nHolders: Int64 = chunk * nChunks
    let burst: Int64 = 200000
    var checksum: Int64 = 0

    for (round in 0..8) {
        checksum = checksum + absorbGarbage(burst, round * 1000000)
    }
    println("PROMOTED_EDGE_WARMUP checksum=${checksum}")

    var chunks = Array<Array<PromoHolder>>(nChunks, {ci =>
        Array<PromoHolder>(chunk, {i => PromoHolder(ci * chunk + i)})
    })
    for (ci in 0..nChunks) {
        for (i in 0..chunk) {
            chunks[ci][i].kid = None
            checksum = checksum + chunks[ci][i].id
        }
    }
    for (round in 0..12) {
        checksum = checksum + absorbGarbage(burst, 20000000 + round * 1000000)
        checksum = checksum + chunks[0][round % chunk].id
    }
    println("PROMOTED_EDGE_AGED checksum=${checksum}")

    for (ci in 0..nChunks) {
        for (i in 0..chunk) {
            let leaf = PromoLeaf(ci * chunk + i + 1)
            chunks[ci][i].kid = leaf
            checksum = checksum + leaf.id
        }
    }
    println("PROMOTED_EDGE_LINKED checksum=${checksum}")

    for (round in 0..40) {
        checksum = checksum + absorbGarbage(burst, 80000000 + round * 1000000)
        let h = chunks[round % nChunks][round % chunk]
        checksum = checksum + h.id
        if (let Some(k) <- h.kid) {
            checksum = checksum + k.id
        }
    }

    var walk: Int64 = 0
    for (ci in 0..nChunks) {
        for (i in 0..chunk) {
            if (let Some(k) <- chunks[ci][i].kid) {
                walk = walk + k.id
            }
        }
    }
    checksum = checksum + walk
    println("PROMOTED_EDGE_OK checksum=${checksum} walk=${walk} holders=${nHolders}")
    return 0
}
