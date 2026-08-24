// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

import std.env.getCommandLine
import std.reflect.PackageInfo
import std.sync.*

foreign func UnloadCJLibrary(libName: CString): Int32

func unloadLibrary(path: String): Int32 {
    let cpath = unsafe { LibC.mallocCString(path) }
    try {
        unsafe { UnloadCJLibrary(cpath) }
    } finally {
        unsafe { LibC.free(cpath) }
    }
}

func runLateLoadProbe(args: Array<String>): Bool {
    let root = PackageInfo.load(args[5])
    let rootName = root.qualifiedName
    let rootVersion = root.version
    let initiallyEmpty = root.subPackages.size == 0

    let branchLoaded = PackageInfo.load(args[3])
    let branch = root.getSubPackage("snapshotprobe.branch")
    let branchPublished = root.subPackages.size == 1 && branch.subPackages.size == 0 &&
        branch.qualifiedName == branchLoaded.qualifiedName

    let leafRLoaded = PackageInfo.load(args[1])
    let leafR = root.getSubPackage("snapshotprobe.branch.r")
    let leafRFromBranch = branch.getSubPackage("snapshotprobe.branch.r")
    let lateAncestorAndChild = root.subPackages.size == 2 && branch.subPackages.size == 1 &&
        leafR == leafRFromBranch && leafR.qualifiedName == leafRLoaded.qualifiedName

    let readerStop = AtomicBool(false)
    let readerValid = AtomicBool(true)
    let observations = AtomicInt64(0)
    let reader = spawn {
        while (!readerStop.load()) {
            let rootCount = root.subPackages.size
            let branchCount = branch.subPackages.size
            if (rootCount < 2 || rootCount > 4 || branchCount < 1 || branchCount > 2) {
                readerValid.store(false)
            }
            observations.fetchAdd(1)
            sleep(Duration.Zero)
        }
        readerValid.load()
    }
    while (observations.load() == 0) {
        sleep(Duration.Zero)
    }

    // Start in reverse argument order. PackageInfo serializes each public load,
    // while the scheduler decides which of these two descendant loads publishes first.
    let siblingFuture = spawn { => PackageInfo.load(args[4]) }
    let leafTFuture = spawn { => PackageInfo.load(args[2]) }
    let siblingLoaded = siblingFuture.get()
    let leafTLoaded = leafTFuture.get()
    readerStop.store(true)
    let interleavingValid = reader.get() && observations.load() > 0

    let branchAgain = root.getSubPackage("snapshotprobe.branch")
    let leafRAgain = root.getSubPackage("snapshotprobe.branch.r")
    let leafT = root.getSubPackage("snapshotprobe.branch.t")
    let leafTFromBranch = branch.getSubPackage("snapshotprobe.branch.t")
    let sibling = root.getSubPackage("snapshotprobe.sibling")
    let completeWithoutDuplicates = root.subPackages.size == 4 && branch.subPackages.size == 2 &&
        branchAgain == branch && leafRAgain == leafR && leafT == leafTFromBranch &&
        leafT.qualifiedName == leafTLoaded.qualifiedName && sibling.qualifiedName == siblingLoaded.qualifiedName &&
        sibling.subPackages.size == 0 && leafT.subPackages.size == 0

    let unloadRc = unloadLibrary(args[1])
    let leafRAfterUnload = root.getSubPackage("snapshotprobe.branch.r")
    let branchLeafRAfterUnload = branch.getSubPackage("snapshotprobe.branch.r")
    let stableAfterUnload = unloadRc == 0 && root.qualifiedName == rootName && root.version == rootVersion &&
        root.subPackages.size == 4 && branch.subPackages.size == 2 && leafRAfterUnload == leafR &&
        branchLeafRAfterUnload == leafR && leafRAfterUnload.qualifiedName == "snapshotprobe.branch.r"

    let passed = initiallyEmpty && branchPublished && lateAncestorAndChild && interleavingValid &&
        completeWithoutDuplicates && stableAfterUnload
    let result = if (passed) { "PASS" } else { "FAIL" }
    println("TEST name=ElfUnload.PackageSnapshotLateLoad result=${result}")
    println("DETAIL initial=${initiallyEmpty} branch=${branchPublished} late=${lateAncestorAndChild} " +
        "interleave=${interleavingValid} observations=${observations.load()} complete=${completeWithoutDuplicates} " +
        "root_subs=${root.subPackages.size} branch_subs=${branch.subPackages.size} unload_rc=${unloadRc} " +
        "stable_after_unload=${stableAfterUnload}")
    passed
}

main(): Int64 {
    let args = getCommandLine()
    if (args.size != 6) {
        println("TEST name=ElfUnload.PackageSnapshotLateLoad result=FAIL")
        println("DETAIL reason=arguments count=${args.size}")
        return 2
    }
    try {
        if (runLateLoadProbe(args)) { 0 } else { 1 }
    } catch (e: Exception) {
        println("TEST name=ElfUnload.PackageSnapshotLateLoad result=FAIL")
        println("DETAIL exception=${e}")
        1
    }
}
