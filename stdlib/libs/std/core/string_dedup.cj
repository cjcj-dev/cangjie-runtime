/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.core

// Only String supplies this ABI: promotion cannot distinguish immutable String
// backing from mutable byte arrays. The pin protects the input during registration.
// L01s: register through the existing pinned-input C ABI. A managed return
// of the canonical RawArray requires a compiler intrinsic (tracked separately).
foreign func CJ_MRT_RequestStringDedup(data: CPointer<UInt8>, length: UIntNative): Unit

func canonicalizeString(candidate: String): String {
    unsafe {
        let data = acquireRawData<UInt8>(candidate.myData)
        try {
            CJ_MRT_RequestStringDedup(data, UIntNative(candidate.length))
        } finally {
            releaseRawData<UInt8>(candidate.myData, data)
        }
    }
    return candidate
}
