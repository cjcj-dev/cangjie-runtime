class SurvivalNode {
    public var id: Int64
    public var next: ?SurvivalNode

    public init(id: Int64) {
        this.id = id
        this.next = None
    }
}

foreign func MRT_NwReadDiagArm(probe: Int64, expected: Int64): Unit
foreign func MRT_NwReadDiagFinish(got: Int64): Unit

func expectedProbeId(cursorAt: Int64, probe: Int64): Int64 {
    if (probe >= cursorAt) {
        return probe
    }
    let ring: Int64 = 1200000
    let write = probe + ((cursorAt - 1 - probe) / ring) * ring
    return (write / 3200) * 10000 + write % 3200
}

main(): Int64 {
    // Keep survival_dense.cj's allocation and root-update recipe unchanged.
    // The only additions are the closed-form expected value and the two
    // default-off calls bracketing the victim probe read.
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
            let probe = Int64((cursor + round) % Int64(roots.size))
            let expected = expectedProbeId(cursor, probe)
            unsafe { MRT_NwReadDiagArm(probe, expected) }
            let got = roots[probe].id
            unsafe { MRT_NwReadDiagFinish(got) }
            checksum = checksum + got
        }
        println("SURVIVAL_WAVE wave=${wave} checksum=${checksum}")
    }

    println("SURVIVAL_DENSE_OK checksum=${checksum} root0=${roots[0].id} cursor=${cursor}")
    return 0
}
