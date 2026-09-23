// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

import std.env.getCommandLine
import std.reflect.PackageInfo

func runSnapshotProbe(args: Array<String>): Bool {
    // Load descendants before their ancestors so every snapshot is built from
    // the same complete package catalog that the legacy prefix scan observes.
    let leafRLoaded = PackageInfo.load(args[1])
    let leafTLoaded = PackageInfo.load(args[2])
    let branchLoaded = PackageInfo.load(args[3])
    let siblingLoaded = PackageInfo.load(args[4])
    let root = PackageInfo.load(args[5])

    let branchFirst = root.getSubPackage("snapshotprobe.branch")
    let branchSecond = root.getSubPackage("snapshotprobe.branch")
    let deepFromRoot = root.getSubPackage("snapshotprobe.branch.r")
    let sibling = root.getSubPackage("snapshotprobe.sibling")
    let leafRFirst = branchFirst.getSubPackage("snapshotprobe.branch.r")
    let leafRSecond = branchFirst.getSubPackage("snapshotprobe.branch.r")
    let leafT = branchFirst.getSubPackage("snapshotprobe.branch.t")
    let branchLoadedRFirst = branchLoaded.getSubPackage("snapshotprobe.branch.r")
    let branchLoadedRSecond = branchLoaded.getSubPackage("snapshotprobe.branch.r")

    let passed = root.subPackages.size == 4 && branchFirst.subPackages.size == 2 &&
        leafRFirst.subPackages.size == 0 && leafT.subPackages.size == 0 && sibling.subPackages.size == 0 &&
        branchLoaded.subPackages.size == 2 && leafRLoaded.subPackages.size == 0 &&
        leafTLoaded.subPackages.size == 0 && siblingLoaded.subPackages.size == 0 &&
        branchFirst == branchSecond && leafRFirst == leafRSecond && deepFromRoot == leafRFirst &&
        branchLoadedRFirst == branchLoadedRSecond &&
        branchFirst.qualifiedName == branchLoaded.qualifiedName &&
        branchLoadedRFirst.qualifiedName == leafRLoaded.qualifiedName &&
        leafRFirst.qualifiedName == leafRLoaded.qualifiedName &&
        leafT.qualifiedName == leafTLoaded.qualifiedName &&
        sibling.qualifiedName == siblingLoaded.qualifiedName
    let result = if (passed) { "PASS" } else { "FAIL" }
    println("TEST name=ElfUnload.PackageSnapshotSubpackages result=${result}")
    println("DETAIL root_subs=${root.subPackages.size} branch_subs=${branchFirst.subPackages.size} " +
        "leaf_r_subs=${leafRFirst.subPackages.size} leaf_t_subs=${leafT.subPackages.size} " +
        "sibling_subs=${sibling.subPackages.size} repeat_branch=${branchFirst == branchSecond} " +
        "repeat_leaf=${leafRFirst == leafRSecond} deep_same=${deepFromRoot == leafRFirst} " +
        "repeat_build_branch_subs=${branchLoaded.subPackages.size} " +
        "repeat_build_lookup=${branchLoadedRFirst == branchLoadedRSecond}")
    passed
}

main(): Int64 {
    let args = getCommandLine()
    if (args.size != 6) {
        println("TEST name=ElfUnload.PackageSnapshotSubpackages result=FAIL")
        println("DETAIL reason=arguments count=${args.size}")
        return 2
    }
    try {
        if (runSnapshotProbe(args)) { 0 } else { 1 }
    } catch (e: Exception) {
        println("TEST name=ElfUnload.PackageSnapshotSubpackages result=FAIL")
        println("DETAIL exception=${e}")
        1
    }
}
