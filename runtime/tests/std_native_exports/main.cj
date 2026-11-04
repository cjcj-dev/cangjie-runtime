// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
import std.console.*
import std.env.*
import std.process.*

main(): Int64 {
    let key = "CJ_R19_NATIVE_EXPORTS_1105"
    let previous = getVariable(key)
    try {
        setVariable(key, "r19")
        let current = Process.current
        let passed = getProcessId() == current.pid && current.pid > 0 &&
            getVariable(key) == Some("r19") && current.getEnv(key) == Some("r19")
        Console.stdOut.write("R19_CONSOLE ${current.pid}\n")
        Console.stdOut.flush()
        getStdOut().write("R19_ENV_CONSOLE ${passed}\n")
        getStdOut().flush()
        println("R19_ASSERT pass=${passed}")
        if (passed) { 0 } else { 19 }
    } finally {
        if (let Some(value) <- previous) {
            setVariable(key, value)
        } else {
            removeVariable(key)
        }
    }
}
