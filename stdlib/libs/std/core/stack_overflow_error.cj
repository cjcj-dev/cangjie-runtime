/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.core

public class StackOverflowError <: Error {
    private let NotFolded: Int64 = 0
    private let TopFolded: Int64 = 1
    private let BottomFolded: Int64 = 2
    private var FoldedFlag: Int64 = NotFolded

    private func getIsFolded() {
        FoldedFlag = capture.fold
    }

    init() {
        super()
        getIsFolded()
    }
    init(message: String) {
        super(message)
        getIsFolded()
    }
    protected override func getClassName(): String {
        return "StackOverflowError"
    }

    public override func printStackTrace(): Unit {
        eprintln("An exception has occurred:")
        eprintln(toString())
        traceElementArray = getStackTrace()
        if (FoldedFlag == TopFolded) {
            eprintln("\t ... Some frames are not displayed ...")
        }
        // When stack overflow occurs, the first frame does not have a valid line number.Therefor the line number is not displayed.
        var packageDelimiter: String = if (traceElementArray[0].declaringClass.isEmpty()) {
            ""
        } else {
            "."
        }
        eprintln(
            "\t at ${traceElementArray[0].declaringClass}${packageDelimiter}${traceElementArray[0].methodName}(${traceElementArray[0].fileName})"
        )
        for (i in 1..traceElementArray.size) {
            packageDelimiter = if (traceElementArray[i].declaringClass.isEmpty()) {
                ""
            } else {
                "."
            }
            eprintln(
                "\t at ${traceElementArray[i].declaringClass}${packageDelimiter}${traceElementArray[i].methodName}(${traceElementArray[i].fileName}:${traceElementArray[i].lineNumber})"
            )
        }
        if (FoldedFlag == BottomFolded) {
            eprintln("\t ... Some frames are not displayed ...")
        }
    }
}
