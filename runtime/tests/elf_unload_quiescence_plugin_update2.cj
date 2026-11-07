// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

package elfunloadprobe_update2

import elfunloadprobe.*

public interface UnloadProbeMutationInterface2 {
    func mutateValue(): Int64
}

extend UnloadProbeTypeWithInterface <: UnloadProbeMutationInterface2 {
    public func mutateValue(): Int64 {
        return this.value + 91
    }
}
