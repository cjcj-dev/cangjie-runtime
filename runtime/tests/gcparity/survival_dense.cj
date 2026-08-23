class SurvivalNode {
    public var id: Int64
    public var next: ?SurvivalNode

    public init(id: Int64) {
        this.id = id
        this.next = None
    }
}

main(): Int64 {
    // The rotating set is intentionally large.  New objects remain directly
    // rooted for 1.2 million later allocations, so they survive collection
    // boundaries instead of becoming same-iteration garbage.
    var roots = Array<SurvivalNode>(1200000, {i => SurvivalNode(i)})
    var cursor: Int64 = 0
    var checksum: Int64 = 0

    for (wave in 0..12) {
        for (roundInWave in 0..400) {
            let round = wave * 400 + roundInWave
            for (i in 0..3200) {
                let id = round * 10000 + i
                let n = SurvivalNode(id)
                let slot = Int64(cursor % Int64(roots.size))
                roots[slot] = n
                checksum = checksum + n.id
                cursor = cursor + 1
            }
            let probe = Int64((cursor + Int64(round)) % Int64(roots.size))
            checksum = checksum + roots[probe].id
        }
        println("SURVIVAL_WAVE wave=${wave} checksum=${checksum}")
    }

    println("SURVIVAL_DENSE_OK checksum=${checksum} root0=${roots[0].id} cursor=${cursor}")
    return 0
}
