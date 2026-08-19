class Node {
    public var id: Int64
    public var next: ?Node
    public var kids: Array<Node>

    public init(id: Int64) {
        this.id = id
        this.next = None
        this.kids = Array<Node>()
    }
}

let NW_ROOTS: Int64 = 512
let NW_SIDE: Int64 = 2048
let NW_ROUNDS_PER_WAVE: Int64 = 300
let NW_WAVES: Int64 = 12
let NW_DUMP_CAP: Int64 = 32

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

func nodeAddr(n: Node): UInt64 {
    unsafe {
        let p = CPointer<Int64>(inout n.id)
        UInt64((CPointer<Byte>(p) - 8).toUIntNative())
    }
}

func nodeHeader(n: Node): UInt64 {
    unsafe {
        let p = CPointer<Int64>(inout n.id)
        CPointer<UInt64>(CPointer<Byte>(p) - 8).read()
    }
}

func expectedWaveChecksum(wave: Int64): Int64 {
    if (wave == 0) {
        return 9188824693200
    }
    if (wave == 1) {
        return 36815499386400
    }
    if (wave == 2) {
        return 82880024079600
    }
    if (wave == 3) {
        return 147382398772800
    }
    if (wave == 4) {
        return 230322623466000
    }
    if (wave == 5) {
        return 331700698159200
    }
    if (wave == 6) {
        return 451516622852400
    }
    if (wave == 7) {
        return 589770397545600
    }
    if (wave == 8) {
        return 746462022238800
    }
    if (wave == 9) {
        return 921591496932000
    }
    if (wave == 10) {
        return 1115158821625200
    }
    1327163996318400
}

func dumpNode(tag: String, slot: Int64, n: Node, expected: Int64): Unit {
    let addr = nodeAddr(n)
    let hdr = nodeHeader(n)
    let state = (hdr >> 48) & UInt64(3)
    let ti = hdr & UInt64(281474976710655)
    eprintln(
        "${tag} slot=${slot} got=${n.id} exp=${expected} d=${expected - n.id} addr=${hexU64(addr)} hdr=${hexU64(hdr)} state=${state} ti=${hexU64(ti)}")
}

func dumpLiveCohort(roots: Array<Node>, side: Array<Node>, chain: ?Node, wave: Int64, checksum: Int64, expected: Int64): Unit {
    let delta = expected - checksum
    let lastRound = wave * NW_ROUNDS_PER_WAVE + (NW_ROUNDS_PER_WAVE - 1)
    eprintln("NW_DRIFT_WAVE wave=${wave} checksum=${checksum} expected=${expected} delta=${delta} lastRound=${lastRound}")
    var mismatch: Int64 = 0
    var dumped: Int64 = 0
    for (rk in 0..64) {
        let exp = lastRound * 1000 + rk
        let got = roots[rk].id
        if (got != exp) {
            mismatch = mismatch + 1
            if (dumped < NW_DUMP_CAP) {
                dumpNode("NW_DRIFT_ROOT", rk, roots[rk], exp)
                dumped = dumped + 1
            }
        }
    }
    var staleTail: Int64 = 0
    for (tk in 64..NW_ROOTS) {
        if (roots[tk].id != tk) {
            staleTail = staleTail + 1
            if (dumped < NW_DUMP_CAP) {
                dumpNode("NW_DRIFT_TAIL", tk, roots[tk], tk)
                dumped = dumped + 1
            }
        }
    }
    var chainLen: Int64 = 0
    var cur = chain
    while (true) {
        if (let Some(n) <- cur) {
            chainLen = chainLen + 1
            cur = n.next
        } else {
            break
        }
    }
    eprintln(
        "NW_DRIFT_SCAN rootLiveMismatch=${mismatch} tailMismatch=${staleTail} chainLen=${chainLen} roots0=${roots[0].id} roots0exp=${lastRound * 1000} side0=${side[0].id}")
}

main(): Int64 {
    var roots: Array<Node> = Array<Node>(NW_ROOTS, {i => Node(i)})
    var side: Array<Node> = Array<Node>(NW_SIDE, {_ => Node(0)})
    var chain: ?Node = None
    var checksum: Int64 = 0
    var dumpedDrift: Bool = false

    for (wave in 0..NW_WAVES) {
        for (roundInWave in 0..NW_ROUNDS_PER_WAVE) {
            let round = wave * NW_ROUNDS_PER_WAVE + roundInWave
            for (k in 0..64) {
                let n = Node(round * 1000 + k)
                n.next = roots[k % roots.size]
                n.kids = Array<Node>(8, {j => Node(round * 10 + j)})
                for (j in 0..8) {
                    n.kids[j] = Node(round * 100 + j)
                }
                roots[k % roots.size] = n
                n.next = chain
                chain = n
                checksum = checksum + n.id
            }
            for (i in 0..NW_SIDE) {
                let y = Node(round * 100000 + i)
                y.next = side[i]
                side[i] = y
                checksum = checksum + y.id
            }
            if (round % 3 == 2) {
                for (i in 0..NW_SIDE) {
                    side[i] = Node(-1)
                }
                if (let Some(c) <- chain) {
                    chain = c.next
                }
            }
            checksum = checksum + roots[0].id
        }
        println("WAVE_DONE wave=${wave} checksum=${checksum}")
        let expected = expectedWaveChecksum(wave)
        if (checksum != expected && !dumpedDrift) {
            dumpLiveCohort(roots, side, chain, wave, checksum, expected)
            dumpedDrift = true
        }
    }

    println("NATURAL_WAVE_OK checksum=${checksum} roots0=${roots[0].id}")
    return 0
}
