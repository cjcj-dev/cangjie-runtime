// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.

import std.collection.concurrent.*

main(args: Array<String>): Int64 {
    match (args[0]) {
        case "blocking" => checkBlockingQueueGrowthRecovery()
        case "linked" => checkLinkedQueueGrowthRecovery()
        case _ => return 2
    }
    return 0
}
