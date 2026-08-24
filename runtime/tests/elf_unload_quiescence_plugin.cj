// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

package elfunloadprobe

foreign func MRT_TestElfUnloadNativeBlock(): Unit
foreign func MRT_TestElfUnloadMaybeNativeBlock(): Unit

func unloadInitBlock(): Int64 {
    unsafe { MRT_TestElfUnloadMaybeNativeBlock() }
    return 1
}

public class UnloadProbeType {
    public let value: Int64

    public init(value: Int64) {
        this.value = value
    }
}

public var unloadStaticRoot: ?UnloadProbeType = UnloadProbeType(7)
public var unloadInitSentinel: Int64 = unloadInitBlock()

public func unloadMarker(): Int64 {
    return 42
}

public func unloadNativeBlock(arg: CPointer<Unit>): CPointer<Unit> {
    unsafe { MRT_TestElfUnloadNativeBlock() }
    return arg
}

public func unloadQueuedTask(arg: CPointer<Unit>): CPointer<Unit> {
    return arg
}

public func unloadSpawnActive(arg: CPointer<Unit>): CPointer<Unit> {
    spawn { => unsafe { MRT_TestElfUnloadNativeBlock() } }
    return arg
}
