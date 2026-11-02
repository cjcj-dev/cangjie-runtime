// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

package elfunloadprobe_reload

foreign func MRT_TestElfUnloadNativeBlock(): Unit
foreign func MRT_TestElfUnloadMaybeNativeBlock(): Unit

public interface UnloadProbeInterface {
    func probeValue(): Int64
}

public class UnloadProbeTypeWithInterface <: UnloadProbeInterface {
    public let value: Int64

    public init(value: Int64) {
        this.value = value
    }

    public func probeValue(): Int64 {
        return value
    }
}

public var unloadInterfaceRoot: ?UnloadProbeTypeWithInterface = UnloadProbeTypeWithInterface(11)

public func makeUnloadProbe(): UnloadProbeTypeWithInterface {
    return UnloadProbeTypeWithInterface(13)
}

public var unloadInitSentinel: Int64 = unsafe {
    MRT_TestElfUnloadMaybeNativeBlock()
    1
}

public func unloadMarker(): Int64 {
    return unloadInitSentinel
}
