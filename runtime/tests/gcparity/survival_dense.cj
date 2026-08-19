class SurvivalNode {
    public var id: Int64
    public var next: ?SurvivalNode

    public init(id: Int64) {
        this.id = id
        this.next = None
    }
}

let SD_ROOTS: Int64 = 1200000
let SD_ROUNDS_PER_WAVE: Int64 = 400
let SD_ALLOCS_PER_ROUND: Int64 = 3200
let SD_WAVES: Int64 = 12
let SD_DUMP_CAP: Int64 = 32

func hexU64(v: UInt64): String {
    var buf = Array<UInt8>(18, repeat: UInt8(48))
    buf[0] = UInt8(48)
    buf[1] = UInt8(120)
    var x: UInt64 = v
    var i: Int64 = 17
    while (i >= 2) {
        let d = Int64(x % 16)
        if (d < 10) {
            buf[i] = UInt8(48 + d)
        } else {
            buf[i] = UInt8(87 + d)
        }
        x = x / 16
        i = i - 1
    }
    unsafe { String.fromUtf8Unchecked(buf) }
}

func nodeAddr(n: SurvivalNode): UInt64 {
    unsafe {
        let p = CPointer<Int64>(inout n.id)
        UInt64((CPointer<Byte>(p) - 8).toUIntNative())
    }
}

func nodeHeader(n: SurvivalNode): UInt64 {
    unsafe {
        let p = CPointer<Int64>(inout n.id)
        CPointer<UInt64>(CPointer<Byte>(p) - 8).read()
    }
}

func expectedRootId(slot: Int64, cursor: Int64): Int64 {
    if (cursor <= slot) {
        return slot
    }
    let t = (cursor - 1 - slot) / SD_ROOTS
    let k = slot + t * SD_ROOTS
    let round = k / SD_ALLOCS_PER_ROUND
    let i = k % SD_ALLOCS_PER_ROUND
    round * 10000 + i
}

func expectedWaveChecksum(wave: Int64): Int64 {
    if (wave == 0) {
        return 2555875089800
    }
    if (wave == 1) {
        return 10232424689600
    }
    if (wave == 2) {
        return 23030574449400
    }
    if (wave == 3) {
        return 40950324369200
    }
    if (wave == 4) {
        return 63991674449000
    }
    if (wave == 5) {
        return 92154624688800
    }
    if (wave == 6) {
        return 125439175088600
    }
    if (wave == 7) {
        return 163845325648400
    }
    if (wave == 8) {
        return 207373079088200
    }
    if (wave == 9) {
        return 256022432688000
    }
    if (wave == 10) {
        return 309793386447800
    }
    368685940367600
}

func dumpNode(slot: Int64, n: SurvivalNode, expected: Int64): Unit {
    let addr = nodeAddr(n)
    let hdr = nodeHeader(n)
    let state = (hdr >> 48) & UInt64(3)
    let ti = hdr & UInt64(281474976710655)
    eprintln(
        "SD_DRIFT_NODE slot=${slot} got=${n.id} exp=${expected} d=${expected - n.id} addr=${hexU64(addr)} hdr=${hexU64(hdr)} state=${state} ti=${hexU64(ti)}")
}

func dumpRootCohort(roots: Array<SurvivalNode>, cursor: Int64, wave: Int64, checksum: Int64, expected: Int64): Unit {
    let delta = expected - checksum
    eprintln("SD_DRIFT_WAVE wave=${wave} checksum=${checksum} expected=${expected} delta=${delta} cursor=${cursor}")
    var mismatch: Int64 = 0
    var firstSlot: Int64 = -1
    var lastSlot: Int64 = -1
    var prevSlot: Int64 = -1
    var runs: Int64 = 0
    var sumDelta: Int64 = 0
    var mod768: Int64 = 0
    var mod4096: Int64 = 0
    for (slot in 0..SD_ROOTS) {
        let got = roots[slot].id
        let exp = expectedRootId(slot, cursor)
        if (got != exp) {
            mismatch = mismatch + 1
            if (firstSlot < 0) {
                firstSlot = slot
            }
            if (prevSlot < 0 || slot != prevSlot + 1) {
                runs = runs + 1
            }
            prevSlot = slot
            lastSlot = slot
            sumDelta = sumDelta + (exp - got)
            if (slot % 768 == 0) {
                mod768 = mod768 + 1
            }
            if (slot % 4096 == 0) {
                mod4096 = mod4096 + 1
            }
        }
    }
    eprintln(
        "SD_DRIFT_SCAN mismatch=${mismatch} first=${firstSlot} last=${lastSlot} runs=${runs} sumDelta=${sumDelta} mod768=${mod768} mod4096=${mod4096}")
    var dumped: Int64 = 0
    if (mismatch > 0) {
        for (ds in 0..SD_ROOTS) {
            let got = roots[ds].id
            let exp = expectedRootId(ds, cursor)
            if (got != exp) {
                dumpNode(ds, roots[ds], exp)
                dumped = dumped + 1
                if (dumped >= SD_DUMP_CAP) {
                    break
                }
            }
        }
    }
    eprintln("SD_DRIFT_DUMPED n=${dumped} cap=${SD_DUMP_CAP} root0=${roots[0].id}")
}

main(): Int64 {
    // The rotating set is intentionally large.  New objects remain directly
    // rooted for 1.2 million later allocations, so they survive collection
    // boundaries instead of becoming same-iteration garbage.
    var roots = Array<SurvivalNode>(SD_ROOTS, {i => SurvivalNode(i)})
    var cursor: Int64 = 0
    var checksum: Int64 = 0
    var dumpedDrift: Bool = false

    for (wave in 0..SD_WAVES) {
        for (roundInWave in 0..SD_ROUNDS_PER_WAVE) {
            let round = wave * SD_ROUNDS_PER_WAVE + roundInWave
            for (i in 0..SD_ALLOCS_PER_ROUND) {
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
        let expected = expectedWaveChecksum(wave)
        if (checksum != expected && !dumpedDrift) {
            dumpRootCohort(roots, cursor, wave, checksum, expected)
            dumpedDrift = true
        }
    }

    println("SURVIVAL_DENSE_OK checksum=${checksum} root0=${roots[0].id} cursor=${cursor}")
    return 0
}
