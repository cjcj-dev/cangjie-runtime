// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

package elfunloadqueueblocker

foreign func MRT_TestElfUnloadNativeBlock(): Unit

public func queueBlocker(arg: CPointer<Unit>): CPointer<Unit> {
    unsafe { MRT_TestElfUnloadNativeBlock() }
    return arg
}
