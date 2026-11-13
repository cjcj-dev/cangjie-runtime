package task_result_wait

foreign func CheckTaskResultWait(mode: Int32): Int32

main(): Int64 {
    for (mode in 0..4) {
        let rc = unsafe { CheckTaskResultWait(Int32(mode)) }
        if (rc != 0) {
            println("TASK_WAIT_FAIL mode=${mode} rc=${rc}")
            return 1
        }
        // C2N must return to a usable managed scheduler after each path.
        let resumed = spawn { 17 }
        if (resumed.get() != 17) { return 2 }
        println("TASK_WAIT_RESTORED mode=${mode}")
    }
    println("TASK_WAIT_ALL_PATHS_OK")
    return 0
}
