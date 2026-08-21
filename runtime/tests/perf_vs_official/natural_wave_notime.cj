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

main(): Int64 {
    var roots: Array<Node> = Array<Node>(512, {i => Node(i)})
    var side: Array<Node> = Array<Node>(2048, {_ => Node(0)})
    var chain: ?Node = None
    var checksum: Int64 = 0

    // 12 waves x 150 rounds (base was 4x150) for wall>=5s
    for (wave in 0..12) {
        for (roundInWave in 0..150) {
            let round = wave * 300 + roundInWave
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
            for (i in 0..2048) {
                let y = Node(round * 100000 + i)
                y.next = side[i]
                side[i] = y
                checksum = checksum + y.id
            }
            if (round % 3 == 2) {
                for (i in 0..2048) {
                    side[i] = Node(-1)
                }
                if (let Some(c) <- chain) {
                    chain = c.next
                }
            }
            checksum = checksum + roots[0].id
        }
        println("WAVE_DONE wave=${wave} checksum=${checksum}")
    }

    println("NATURAL_WAVE_OK checksum=${checksum} roots0=${roots[0].id}")
    return 0
}
