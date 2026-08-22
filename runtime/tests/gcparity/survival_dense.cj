class SurvivalNode {
    public var id: Int64
    public var next: ?SurvivalNode

    public init(id: Int64) {
        this.id = id
        this.next = None
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

foreign func sddrift_slot_ref(rawArray: UInt64, slot: Int64): UInt64
foreign func sddrift_dump_node(rawArray: UInt64, slot: Int64, got: Int64, exp: Int64, lastRef: UInt64): Unit
foreign func MCC_SdDriftDescribe(curRef: UInt64, lastRef: UInt64, got: Int64, exp: Int64): Unit
foreign func MCC_GetGCCount(): UIntNative

let SD_ROOTS: Int64 = 1200000
let SD_ROUNDS_PER_WAVE: Int64 = 400
let SD_ALLOCS_PER_ROUND: Int64 = 3200
let SD_WAVES: Int64 = 12

func arrayRawAddr<T>(arr: Array<T>): UInt64 {
    var box = ArrBox<T>(arr, 0)
    unsafe {
        let mp = CPointer<Int64>(inout box.marker)
        let base = CPointer<UInt64>(CPointer<Byte>(mp) - 24)
        base.read(0)
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

main(): Int64 {
    var roots = Array<SurvivalNode>(SD_ROOTS, {i => SurvivalNode(i)})
    var lastRef = Array<UInt64>(SD_ROOTS, {_ => 0})
    var lastGc = Array<Int64>(SD_ROOTS, {_ => 0})
    let raw = arrayRawAddr(roots)
    var cursor: Int64 = 0
    var checksum: Int64 = 0
    var dumpedProbe: Bool = false

    for (wave in 0..SD_WAVES) {
        for (roundInWave in 0..SD_ROUNDS_PER_WAVE) {
            let round = wave * SD_ROUNDS_PER_WAVE + roundInWave
            for (i in 0..SD_ALLOCS_PER_ROUND) {
                let id = round * 10000 + i
                let n = SurvivalNode(id)
                let slot = Int64(cursor % Int64(roots.size))
                roots[slot] = n
                unsafe {
                    lastRef[slot] = sddrift_slot_ref(raw, slot)
                    lastGc[slot] = Int64(MCC_GetGCCount())
                }
                checksum = checksum + n.id
                cursor = cursor + 1
            }
            let probe = Int64((cursor + Int64(round)) % Int64(roots.size))
            let got = roots[probe].id
            let expProbe = expectedRootId(probe, cursor)
            checksum = checksum + got
            if (got != expProbe && !dumpedProbe) {
                dumpedProbe = true
                let writtenGc = lastGc[probe]
                let nowGc = unsafe { MCC_GetGCCount() }
                eprintln(
                    "SD_DRIFT_PROBE wave=${wave} round=${round} probe=${probe} got=${got} exp=${expProbe} d=${expProbe - got} cursor=${cursor} writeGc=${writtenGc} nowGc=${nowGc}")
                unsafe {
                    sddrift_dump_node(raw, probe, got, expProbe, lastRef[probe])
                    MCC_SdDriftDescribe(sddrift_slot_ref(raw, probe), lastRef[probe], got, expProbe)
                }
            }
        }
        println("SURVIVAL_WAVE wave=${wave} checksum=${checksum}")
        let expected = expectedWaveChecksum(wave)
        if (checksum != expected) {
            eprintln("SD_DRIFT_WAVE wave=${wave} checksum=${checksum} expected=${expected} d=${expected - checksum}")
        }
    }

    println("SURVIVAL_DENSE_OK checksum=${checksum} root0=${roots[0].id} cursor=${cursor}")
    return 0
}
