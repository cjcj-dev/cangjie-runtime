package finalizer_thread_data

import std.collection.*

foreign {
    func PrepareThreadDataProbe(): Int64
    func RequireCurrentThreadData(): Int64
}

class Tracked {
    let id: Int64
    init(id: Int64) {
        this.id = id
    }
    ~init {}
}

func workload(rounds: Int64): Int64 {
    var total = 0
    for (i in 0..rounds) {
        let tracked = Tracked(i)
        total += tracked.id & 1
        unsafe { RequireCurrentThreadData() }
    }
    return total
}

main(args: Array<String>): Int64 {
    if (unsafe { PrepareThreadDataProbe() } != 0) {
        return 100
    }
    let threads = Int64.parse(args[0])
    let rounds = Int64.parse(args[1])
    let workers = ArrayList<Future<Int64>>()
    for (_ in 0..threads) {
        workers.add(spawn { workload(rounds) })
    }
    var total = 0
    for (worker in workers) {
        total += worker.get()
    }
    println("FINALIZER_THREAD_DATA_DONE threads=${threads} rounds=${rounds} total=${total}")
    return 0
}
