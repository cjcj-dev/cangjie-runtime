class IdleKid {
    public var id: Int64
    public init(id: Int64) {
        this.id = id
    }
}

class IdleHolder {
    public var id: Int64
    public var kid: IdleKid
    public init(id: Int64) {
        this.id = id
        this.kid = IdleKid(id)
    }
}

main(): Int64 {
    let nHolders: Int64 = 8192
    var holders = Array<IdleHolder>(nHolders, {i => IdleHolder(i)})
    var checksum: Int64 = 0
    for (i in 0..nHolders) {
        checksum = checksum + holders[i].id
    }
    var junk = Array<IdleKid>(200000, {j => IdleKid(-(j + 1))})
    checksum = checksum + junk[0].id
    junk = Array<IdleKid>(1, {_ => IdleKid(0)})
    println("IDLE_AGED checksum=${checksum}")
    sleep(4 * Duration.second)
    for (i in 0..nHolders) {
        let kid = IdleKid(1000000 + i)
        holders[i].kid = kid
        checksum = checksum + kid.id
    }
    println("IDLE_WRITTEN checksum=${checksum}")
    junk = Array<IdleKid>(200000, {j => IdleKid(-(200000 + j))})
    checksum = checksum + junk[0].id
    println("IDLE_LONGWINDOW_OK checksum=${checksum} holder0=${holders[0].id}")
    return 0
}
