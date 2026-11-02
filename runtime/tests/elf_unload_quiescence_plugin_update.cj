// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

package elfunloadprobe_update0

import elfunloadprobe

public interface UnloadProbeMutationInterface0 {
    func mutateValue(): Int64
}

// Loading this image is the real late metadata update: the loader publishes
// an extension for the already-loaded class and the runtime adds its mtable
// entry while readers may still hold a stable snapshot.
extend UnloadProbeTypeWithInterface <: UnloadProbeMutationInterface0 {
    public func mutateValue(): Int64 {
        return this.value + 89
    }
}
