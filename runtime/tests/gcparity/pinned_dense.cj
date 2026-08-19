import std.collection.ArrayList
import std.sync.Mutex

class PinKid {
    public var id: Int64
    public init(id: Int64) {
        this.id = id
    }
}

class PinHolder {
    public var id: Int64
    public var kid: PinKid
    public var mtx: Mutex
    public init(id: Int64) {
        this.id = id
        this.kid = PinKid(id)
        this.mtx = Mutex()
    }
}

main(): Int64 {
    let nHolders: Int64 = 4096
    var holders = Array<PinHolder>(nHolders, {i => PinHolder(i)})
    var checksum: Int64 = 0
    for (wave in 0..8) {
        let waveF = ArrayList<Future<Int64>>()
        for (i in 0..256) {
            let captured = PinKid(wave * 1000 + i)
            waveF.add(spawn {
                =>
                    sleep(Duration.millisecond)
                    captured.id
            })
        }
        for (f in waveF) {
            checksum = checksum + f.get()
        }
        for (i in 0..nHolders) {
            let kid = PinKid(wave * 100000 + i)
            holders[i].kid = kid
            checksum = checksum + kid.id
        }
        var junk = Array<PinKid>(80000, {j => PinKid(-(j + 1))})
        checksum = checksum + junk[0].id
        println("PINNED_WAVE wave=${wave} checksum=${checksum}")
    }
    println("PINNED_DENSE_OK checksum=${checksum} holders=${holders[0].id}")
    return 0
}
