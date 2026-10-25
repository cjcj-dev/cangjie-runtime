// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
import std.collection.*

main(): Int64 {
    let tree = TreeMap<Int64, Int64>()
    println("EMPTY_TREE_TARGET entered=1")
    for (inclusive in [true, false]) {
        if (let Some(_) <- tree.forward(0, inclusive: inclusive).next()) {
            return 1
        }
        if (let Some(_) <- tree.backward(0, inclusive: inclusive).next()) {
            return 2
        }
    }
    println("EMPTY_TREE_TARGET completed=1")
    return 0
}
