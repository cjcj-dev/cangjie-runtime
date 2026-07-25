// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

package mtable_unload_extension

public interface MTableUnloadProbe {
    func value(): Int64
}

extend Int64 <: MTableUnloadProbe {
    public func value(): Int64 {
        this + 100
    }
}

@C
public func MTableUnloadDispatch(value: Int64): Int64 {
    let probe: MTableUnloadProbe = value
    probe.value()
}
