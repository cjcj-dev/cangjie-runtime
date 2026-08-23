/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.core

private const STRING_CANONICAL_INITIAL_CAPACITY: Int64 = 16

/*
 * String is a value struct, so there is no stable String object that a weak
 * runtime queue can update in place. This table implements the explicit,
 * functional String.deduplicated() contract. Entries are deliberately strong:
 * until a managed-reference ABI exists for a weak backing table, every unique
 * value passed here remains canonical for the process lifetime.
 */
private class StringCanonicalTable {
    private let lockState = AtomicInt64(0)
    private var entries = Array<String>(STRING_CANONICAL_INITIAL_CAPACITY, repeat: String.empty)
    private var entryCount: Int64 = 0

    private func lock(): Unit {
        while (!lockState.compareAndSwap(0, 1)) {
            intrinsicSleep(0)
        }
    }

    private func unlock(): Unit {
        lockState.store(0)
    }

    private func grow(): Unit {
        let grown = Array<String>(entries.size * 2, repeat: String.empty)
        entries.copyTo(grown, 0, 0, entryCount)
        entries = grown
    }

    func canonicalize(candidate: String): String {
        lock()
        try {
            for (i in 0..entryCount) {
                let known = entries[i]
                if (known == candidate) {
                    return known
                }
            }
            if (entryCount == entries.size) {
                grow()
            }
            entries[entryCount] = candidate
            entryCount++
            return candidate
        } finally {
            unlock()
        }
    }
}

private let STRING_CANONICAL_TABLE = StringCanonicalTable()

func canonicalizeString(candidate: String): String {
    STRING_CANONICAL_TABLE.canonicalize(candidate)
}
