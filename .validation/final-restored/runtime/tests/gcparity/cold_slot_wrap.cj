import std.runtime.*

class ColdLeaf {
    public var id: Int64

    public init(id: Int64) {
        this.id = id
    }
}

class ColdHolder {
    public var id: Int64
    public var kid: ?ColdLeaf

    public init(id: Int64) {
        this.id = id
        this.kid = None
    }
}

main(): Int64 {
    let nHolders: Int64 = 4096
    var holders = Array<ColdHolder>(nHolders, {i => ColdHolder(i)})
    for (i in 0..nHolders) {
        holders[i].kid = ColdLeaf(i + 1)
    }
    println("COLD_SLOT_LINKED holders=${nHolders}")

    let junkSlots: Int64 = 256
    var junk = Array<ColdLeaf>(junkSlots, {i => ColdLeaf(-(i + 1))})
    var cursor: Int64 = 0
    var checksum: Int64 = 0

    for (wave in 0..4) {
        gc(heavy: true)
        for (round in 0..200) {
            for (i in 0..1600) {
                let n = ColdLeaf(wave * 1000000 + round * 10000 + i)
                junk[cursor % junkSlots] = n
                checksum = checksum + n.id
                cursor = cursor + 1
            }
        }
        println("COLD_SLOT_WAVE wave=${wave} checksum=${checksum}")
    }

    var walk: Int64 = 0
    var missing: Int64 = 0
    for (i in 0..nHolders) {
        if (let Some(k) <- holders[i].kid) {
            walk = walk + k.id
        } else {
            missing = missing + 1
        }
    }
    checksum = checksum + walk + junk[0].id
    println("COLD_SLOT_OK checksum=${walk} missing=${missing} holders=${nHolders} acc=${checksum}")
    return 0
}
