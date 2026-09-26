/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.core

@Intrinsic
func canonicalizeStringBacking(candidate: RawArray<UInt8>): RawArray<UInt8>

func canonicalizeString(candidate: String): String {
    let backing = canonicalizeStringBacking(candidate.myData)
    return String(backing, 0u32, candidate.length)
}
