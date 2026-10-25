// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
import std.collection.*

main(): Int64 {
    let list = LinkedList<Int64>()
    let first = list.addLast(10)
    let middle = list.addLast(20)
    let last = list.addLast(30)
    list.clear()
    var rejected = 0
    for (node in [first, middle, last]) {
        try {
            let value = node.value
            println("STALE_NODE_VALUE ${value}")
        } catch (_: IllegalStateException) {
            rejected++
        }
    }
    println("LINKED_CLEAR_TARGET executed=1 rejected=${rejected} size=${list.size}")
    if (rejected != 3 || !list.isEmpty()) {
        return 1
    }
    list.addFirst(40)
    if (list.removeLast() != Some(40)) {
        return 2
    }
    return 0
}
