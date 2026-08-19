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

struct ArrBox<T> {
    var arr: Array<T>
    var marker: Int64
    public init(arr: Array<T>, marker: Int64) {
        this.arr = arr
        this.marker = marker
    }
}

foreign func nwdrift_dump_node(rawArray: UInt64, slot: Int64, got: Int64, exp: Int64): Unit

let NW_ROOTS: Int64 = 512
let NW_SIDE: Int64 = 2048
let NW_ROUNDS_PER_WAVE: Int64 = 300
let NW_WAVES: Int64 = 12
let NW_DUMP_CAP: Int64 = 32

func arrayRawAddr<T>(arr: Array<T>): UInt64 {
    var box = ArrBox<T>(arr, 0)
    unsafe {
        let mp = CPointer<Int64>(inout box.marker)
        let base = CPointer<UInt64>(CPointer<Byte>(mp) - 24)
        base.read(0)
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

func dumpLiveCohort(roots: Array<Node>, raw: UInt64, wave: Int64, checksum: Int64, expected: Int64): Unit {
    let delta = expected - checksum
    let lastRound = wave * NW_ROUNDS_PER_WAVE + (NW_ROUNDS_PER_WAVE - 1)
    eprintln("NW_DRIFT_WAVE wave=${wave} checksum=${checksum} expected=${expected} delta=${delta} lastRound=${lastRound}")
    var mismatch: Int64 = 0
    var dumped: Int64 = 0
    let rootsExp = lastRound * 1000
    for (rk in 0..64) {
        let exp = lastRound * 1000 + rk
        let got = roots[rk].id
        if (got != exp) {
            mismatch = mismatch + 1
            if (dumped < NW_DUMP_CAP) {
                unsafe { nwdrift_dump_node(raw, rk, got, exp) }
                dumped = dumped + 1
            }
        }
    }
    var staleTail: Int64 = 0
    for (tk in 64..NW_ROOTS) {
        if (roots[tk].id != tk) {
            staleTail = staleTail + 1
            if (dumped < NW_DUMP_CAP) {
                unsafe { nwdrift_dump_node(raw, tk, roots[tk].id, tk) }
                dumped = dumped + 1
            }
        }
    }
    eprintln(
        "NW_DRIFT_SCAN rootLiveMismatch=${mismatch} tailMismatch=${staleTail} dumped=${dumped} roots0=${roots[0].id} roots0exp=${rootsExp}")
}

main(): Int64 {
    var roots: Array<Node> = Array<Node>(NW_ROOTS, {i => Node(i)})
    var side: Array<Node> = Array<Node>(NW_SIDE, {_ => Node(0)})
    var chain: ?Node = None
    var checksum: Int64 = 0
    var dumpedProbe: Bool = false
    var dumpedWave: Bool = false

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
            let got0 = roots[0].id
            let exp0 = round * 1000
            checksum = checksum + got0
            if (got0 != exp0 && !dumpedProbe) {
                let raw = arrayRawAddr(roots)
                eprintln("NW_DRIFT_PROBE wave=${wave} round=${round} got=${got0} exp=${exp0} d=${exp0 - got0}")
                unsafe { nwdrift_dump_node(raw, 0, got0, exp0) }
                dumpedProbe = true
            }
        }
        println("WAVE_DONE wave=${wave} checksum=${checksum}")
        let expected = expectedWaveChecksum(wave)
        if (checksum != expected && !dumpedWave) {
            let raw = arrayRawAddr(roots)
            dumpLiveCohort(roots, raw, wave, checksum, expected)
            dumpedWave = true
        }
    }

    println("NATURAL_WAVE_OK checksum=${checksum} roots0=${roots[0].id}")
    return 0
}
