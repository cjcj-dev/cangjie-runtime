// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
import std.collection.*
import std.sort.*
import std.unicode.*

func copyToControl(): Bool {
    let source: Array<Int64> = [10, 20, 30]
    let target: Array<Int64> = [0, 0]
    // Check exact messages and the precedence of overlapping invalid arguments.
    let errors: Array<(Int64, Int64, Int64, Bool, String)> = [
        (-1, -1, -1, true, "Invalid argument 'copyLen': expected a non-negative value, got '-1'."),
        (-1, -2, 1, false, "Index out of bounds: negative index, srcStart is '-1', dstStart is '-2'."),
        (3, 2, 1, false, "Index out of bounds: srcStart '3' is greater than or equal to array size '3'."),
        (0, 2, 1, false, "Index out of bounds: dstStart '2' is greater than or equal to target array size '2'."),
        (1, 1, 2, false, "Copy length out of bounds: copyLen is '2', but available source length is '2' and available destination length is '1'."),
        (0, 0, Int64.Max, false, "Copy length out of bounds: copyLen is '9223372036854775807', but available source length is '3' and available destination length is '2'."),
        (Int64.Max, 0, 1, false, "Index out of bounds: srcStart '9223372036854775807' is greater than or equal to array size '3'.")
    ]
    var checked = 0
    for ((src, dst, count, illegal, message) in errors) {
        try {
            source.copyTo(target, src, dst, count)
            return false
        } catch (e: IllegalArgumentException) {
            println("COPYTO_MESSAGE_ASSERT case=${checked} illegal=true")
            if (!illegal || e.message != message) { return false }
        } catch (e: IndexOutOfBoundsException) {
            println("COPYTO_MESSAGE_ASSERT case=${checked} illegal=false")
            if (illegal || e.message != message) { return false }
        }
        checked++
    }
    // Zero count must return before validating even extreme indices or empty arrays.
    source.copyTo(target, Int64.Min, Int64.Max, 0)
    let empty = Array<Int64>()
    empty.copyTo(empty, -1, -1, 0)
    if (target != [0, 0]) { return false }
    source.copyTo(target, 1, 0, 2)
    if (target != [20, 30]) { return false }
    let overlap: Array<String> = ["a", "b", "c", "d"]
    overlap.copyTo(overlap, 0, 1, 3)
    if (overlap != ["a", "a", "b", "c"]) { return false }
    overlap.copyTo(overlap, 1, 0, 3)
    if (overlap != ["a", "b", "c", "c"]) { return false }
    let slice = source[1..3]
    slice.copyTo(target, 0, 0, 2)
    if (target != [20, 30]) { return false }
    println("COPYTO_CONTROL messages=${checked} zero=2 normal=1 overlap=2 slice=1")
    return true
}

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
    if (!copyToControl()) { return 8 }
    println("COLLECTION_CONTROL completed=1")
    return 0
}
