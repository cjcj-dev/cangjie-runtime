// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

class StringDedupChecks {
    private var failures: Int64 = 0

    func check(name: String, passed: Bool): Unit {
        println("STRING_DEDUP_CASE name=${name} pass=${passed}")
        if (!passed) {
            failures++
        }
    }

    func result(): Int64 {
        failures
    }
}

func firstByte(value: String): Byte {
    value[0]
}

func overwriteFirstByte(value: String, byte: Byte): Unit {
    unsafe {
        let data = value.rawData()
        data[0] = byte
    }
}

main(): Int64 {
    let checks = StringDedupChecks()

    checks.check("empty_bypass", String.empty.deduplicated() == String.empty)

    let fullLeft = "full-canonical-one".clone().deduplicated()
    let fullRight = "full-canonical-one".clone().deduplicated()
    checks.check("full_content", fullLeft == fullRight)
    overwriteFirstByte(fullLeft, UInt8(70))
    checks.check("full_equal_backing", firstByte(fullRight) == UInt8(70))

    let unequalLeft = "unequal-left-one".clone().deduplicated()
    let unequalRight = "unequal-right-two".clone().deduplicated()
    checks.check("unequal_content", unequalLeft != unequalRight)
    let unequalRightBefore = firstByte(unequalRight)
    overwriteFirstByte(unequalLeft, UInt8(85))
    checks.check("unequal_not_merged", firstByte(unequalRight) == unequalRightBefore)

    let rangeBaseLeft = "aaRANGE-CANONzz".clone()
    let rangeBaseRight = "bbRANGE-CANONyy".clone()
    let rangeLeft = rangeBaseLeft[2..13].deduplicated()
    let rangeRight = rangeBaseRight[2..13].deduplicated()
    checks.check("range_content", rangeLeft == "RANGE-CANON")
    overwriteFirstByte(rangeLeft, UInt8(81))
    checks.check("range_equal_backing", firstByte(rangeRight) == UInt8(81))

    let largeBacking = ("0123456789" * 64).clone()
    let smallSlice = largeBacking[101..108].deduplicated()
    checks.check("slice_content", smallSlice == "1234567")
    unsafe {
        let largeData = largeBacking.rawData()
        largeData[101] = UInt8(88)
    }
    checks.check("slice_detached", firstByte(smallSlice) == UInt8(49))

    let requests = Array<Future<String>>(32,
        {_ => spawn { => "concurrent-canonical".clone().deduplicated() }})
    let concurrent = Array<String>(32, {i => requests[i].get()})
    var concurrentContent = true
    for (value in concurrent) {
        concurrentContent = concurrentContent && value == "concurrent-canonical"
    }
    checks.check("concurrent_content", concurrentContent)
    overwriteFirstByte(concurrent[0], UInt8(67))
    var concurrentBacking = true
    for (value in concurrent) {
        concurrentBacking = concurrentBacking && firstByte(value) == UInt8(67)
    }
    checks.check("concurrent_equal_backing", concurrentBacking)

    let growAnchor = "grow-canonical-anchor".clone().deduplicated()
    for (i in 0..24) {
        let _filler = "grow-filler-${i}".clone().deduplicated()
    }
    let growAnchorAgain = "grow-canonical-anchor".clone().deduplicated()
    overwriteFirstByte(growAnchor, UInt8(71))
    checks.check("grow_preserves_backing", firstByte(growAnchorAgain) == UInt8(71))

    let failures = checks.result()
    println("STRING_DEDUP_RESULT failures=${failures}")
    return if (failures == 0) { 0 } else { failures }
}
