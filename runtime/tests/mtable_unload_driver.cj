/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package mtable_unload_driver

@When[os == "Linux"]
foreign {
    func dlopen(filename: CString, flag: Int32): CPointer<Unit>
    func dlsym(handle: CPointer<Unit>, symbol: CString): CPointer<Unit>
    func dlclose(handle: CPointer<Unit>): Int32
}

@When[os == "Linux"]
unsafe func RunUnloadCycle(path: String, cycle: Int64): Int64 {
    let pathCString = LibC.mallocCString(path)
    let handle = dlopen(pathCString, 0x2)
    LibC.free(pathCString)
    if (handle.isNull()) {
        println("CYCLE=${cycle} OPEN=FAIL")
        return 1
    }

    let missingCString = LibC.mallocCString("MTableUnloadMissing")
    let missing = dlsym(handle, missingCString)
    LibC.free(missingCString)
    if (!missing.isNull()) {
        let closeResult = dlclose(handle)
        println("CYCLE=${cycle} MISSING=false CLOSE=${closeResult}")
        return 2
    }

    let symbolCString = LibC.mallocCString("MTableUnloadDispatch")
    let symbol = dlsym(handle, symbolCString)
    LibC.free(symbolCString)
    if (symbol.isNull()) {
        let closeResult = dlclose(handle)
        println("CYCLE=${cycle} SYMBOL=FAIL CLOSE=${closeResult}")
        return 3
    }

    let dispatch = CFunc<(Int64) -> Int64>(symbol)
    let first = dispatch(7)
    let second = dispatch(8)
    let closeResult = dlclose(handle)
    let passed = first == 107 && second == 108 && closeResult == 0
    println("CYCLE=${cycle} FIRST=${first} SECOND=${second} MISSING=true CLOSE=${closeResult}")
    if (passed) {
        return 0
    }
    return 4
}

@When[os == "Linux"]
main(): Int64 {
    var failures: Int64 = 0
    unsafe {
        for (cycle in 0..20) {
            failures += RunUnloadCycle("./libmtable_unload_extension.so", cycle)
        }
    }
    let passed = failures == 0
    println("MTABLE_UNLOAD cycles=20 failures=${failures} result=${if (passed) { "PASS" } else { "FAIL" }}")
    if (passed) {
        return 0
    }
    return 1
}
