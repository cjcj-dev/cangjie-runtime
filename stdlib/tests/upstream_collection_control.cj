// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
import std.collection.*
import std.sort.*
import std.unicode.*

main(): Int64 {
    // Wrap the deque before forcing growth through the new addLast path.
    let deque = ArrayDeque<Int64>(8)
    for (i in 0..7) { deque.addLast(i) }
    for (i in 0..5) {
        if (deque.removeFirst() != Some(i)) { return 1 }
    }
    for (i in 7..40) { deque.addLast(i) }
    for (i in 5..40) {
        if (deque.removeFirst() != Some(i)) { return 2 }
    }
    if (!deque.isEmpty()) { return 3 }

    let map = HashMap<Int64, Int64>()
    for (i in 0..80) { map.add(i, i * 2) }
    for (i in 0..40) {
        if (map.remove(i) != Some(i * 2) || map.contains(i)) { return 4 }
    }
    let mapped = map.mapValues { value: Int64 => value + 1 }
    if (mapped.size != 40 || mapped.get(79) != Some(159)) { return 5 }

    let stable: Array<Int64> = [8, -1, 3, 3, 0]
    let unstable: Array<Int64> = [8, -1, 3, 3, 0]
    stableSort(stable)
    unstableSort(unstable)
    let expected: Array<Int64> = [-1, 0, 3, 3, 8]
    for (i in 0..expected.size) {
        if (stable[i] != expected[i] || unstable[i] != expected[i]) { return 6 }
    }
    if (!" \t\u{3000}\n".isBlank() || " \u{3000}x".isBlank()) { return 7 }
    println("COLLECTION_CONTROL completed=1")
    return 0
}
