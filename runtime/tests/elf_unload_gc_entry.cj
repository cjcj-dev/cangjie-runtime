// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

import std.sync.AtomicBool
import std.collection.ArrayList

foreign func CJ_MRT_ForceFullGC(): Unit
foreign func MRT_TestElfUnloadGcEntryCount(): UInt64

class ElfUnloadGcRoot {
    let value: Int64

    init(value: Int64) {
        this.value = value
    }
}

let gcRootEntered = AtomicBool(false)
let gcRootRelease = AtomicBool(false)

func KeepManagedRoot(): Int64 {
    var root = ElfUnloadGcRoot(1)
    var value: Int64 = 1
    // One deterministic allocation wave crosses the product young-generation
    // trigger, so Mark::VisitMinorRootSlots must enumerate the managed stack.
    let roots = ArrayList<ElfUnloadGcRoot>()
    for (index in 0..1200000) {
        roots.add(ElfUnloadGcRoot(index))
    }
    gcRootEntered.store(true)
    while (!gcRootRelease.load()) {
        value++
        root = ElfUnloadGcRoot(value)
    }
    root.value + roots.size
}

main(): Int64 {
    let before = unsafe { MRT_TestElfUnloadGcEntryCount() }
    let holder = spawn { => KeepManagedRoot() }
    while (!gcRootEntered.load()) {
        sleep(Duration.Zero)
    }
    unsafe { CJ_MRT_ForceFullGC() }
    let after = unsafe { MRT_TestElfUnloadGcEntryCount() }
    gcRootRelease.store(true)
    let heldValue = holder.get()
    let passed = after > before && heldValue > 0
    let result = if (passed) { "PASS" } else { "FAIL" }
    println("TEST name=ElfUnload.GcEntryReader result=${result}")
    println("DETAIL runtime_entry=1 managed_root=${heldValue} gc_entries=${before}/${after}")
    if (passed) { 0 } else { 1 }
}
