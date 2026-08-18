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

func churn(junk: Array<PromoLeaf>, startId: Int64, rounds: Int64, inner: Int64): Int64 {
    var cursor: Int64 = 0
    let slots = Int64(junk.size)
    for (round in 0..rounds) {
        for (i in 0..inner) {
            junk[cursor % slots] = PromoLeaf(startId + round * inner + i)
            cursor = cursor + 1
        }
    }
    return junk[0].id
}

main(): Int64 {
    // Compile with cjEnableGC unset.
    //
    // After A flip-promotes, the next TakeRegion fires HEU if live >=
    // heapThreshold and youngAllocated is still 0. IsFrequentHeuristicGC
    // gives a 200ms window (GcRequest.cpp) to hit the 32MiB young line
    // first. A tight 38MiB churn after link forces that young so B is
    // marked only via remset (FYS=false), before HEU can full-scan-rescue.
    //
    // Final checksum is only the walk of B (sum 1..nHolders). Junk ids
    // are timing-dependent and must not enter the golden.
    let junkSlots: Int64 = 256
    let waveRounds: Int64 = 600
    let waveInner: Int64 = 3200
    var junk = Array<PromoLeaf>(junkSlots, {i => PromoLeaf(-(i + 1))})

    (void)churn(junk, 0, waveRounds, waveInner)
    println("PROMOTED_EDGE_WARMUP")

    var pad = Array<PromoLeaf>(8192, {i => PromoLeaf(-10000 - i)})
    pad = Array<PromoLeaf>(1, {_ => PromoLeaf(0)})

    let nHolders: Int64 = 2048
    var holders = Array<PromoHolder>(nHolders, {i => PromoHolder(i)})
    for (i in 0..nHolders) {
        holders[i].kid = None
    }

    pad = Array<PromoLeaf>(8192, {i => PromoLeaf(-20000 - i)})
    pad = Array<PromoLeaf>(1, {_ => PromoLeaf(0)})

    (void)churn(junk, 20000000, waveRounds, waveInner)
    println("PROMOTED_EDGE_AGED")

    pad = Array<PromoLeaf>(8192, {i => PromoLeaf(-30000 - i)})
    pad = Array<PromoLeaf>(1, {_ => PromoLeaf(0)})

    for (i in 0..nHolders) {
        holders[i].kid = PromoLeaf(i + 1)
    }
    println("PROMOTED_EDGE_LINKED")

    pad = Array<PromoLeaf>(8192, {i => PromoLeaf(-40000 - i)})
    pad = Array<PromoLeaf>(1, {_ => PromoLeaf(0)})

    (void)churn(junk, 40000000, waveRounds, waveInner)
    println("PROMOTED_EDGE_PROMOTE")

    (void)churn(junk, 80000000, waveRounds, waveInner)
    println("PROMOTED_EDGE_RESCAN")

    (void)churn(junk, 120000000, waveRounds, waveInner)
    println("PROMOTED_EDGE_COPY")

    var walk: Int64 = 0
    var missing: Int64 = 0
    for (i in 0..nHolders) {
        if (let Some(k) <- holders[i].kid) {
            walk = walk + k.id
        } else {
            missing = missing + 1
        }
    }
    println("PROMOTED_EDGE_OK checksum=${walk} missing=${missing} holders=${nHolders}")
    return 0
}
