class RefNode {
    public var next: ?RefNode

    public init() {
        this.next = None
    }
}

main(): Int64 {
    // Allocate the graph once; the timed work below only rewrites references.
    let count: Int64 = 65536
    var left = Array<RefNode>(count, {_ => RefNode()})
    var right = Array<RefNode>(count, {_ => RefNode()})
    var checksum: Int64 = 0

    for (i in 0..count) {
        left[i].next = right[(i + 1) % count]
    }
    for (wave in 0..1000) {
        for (i in 0..count) {
            left[i].next = right[(i + wave) % count]
        }
        if (let Some(_) <- left[wave % count].next) {
            checksum = checksum + 1
        }
    }

    println("REF_REWRITE_OK checksum=${checksum}")
    return 0
}
