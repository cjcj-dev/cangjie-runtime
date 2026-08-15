class AllocationNode {
    public var id: Int64
    public var next: ?AllocationNode

    public init(id: Int64) {
        this.id = id
        this.next = None
    }
}

main(): Int64 {
    // This is the only deliberately long-lived cohort.  Almost all subsequent
    // allocations die in the iteration that creates them.
    let oldCohort = Array<AllocationNode>(4096, {i => AllocationNode(i)})
    var checksum: Int64 = 0

    for (wave in 0..12) {
        for (roundInWave in 0..400) {
            let round = wave * 400 + roundInWave
            for (i in 0..3200) {
                let young = AllocationNode(round * 10000 + i)
                checksum = checksum + young.id
            }
            checksum = checksum + oldCohort[round % oldCohort.size].id
        }
        println("ALLOCATION_WAVE wave=${wave} checksum=${checksum}")
    }

    println("ALLOCATION_DENSE_OK checksum=${checksum} old0=${oldCohort[0].id}")
    return 0
}
