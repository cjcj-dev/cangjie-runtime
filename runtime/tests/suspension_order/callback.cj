// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
foreign func R06Invoke(callback: CFunc<() -> Int64>, stopped: Bool): Int64
foreign func R06Reached(): Int64

@C
func r06Callback(): Int64 {
    // First managed instruction reached via the generated N2C callback wrapper.
    unsafe { R06Reached() }
}

main(): Int64 {
    let control = unsafe { R06Invoke(r06Callback, false) }
    let stopped = unsafe { R06Invoke(r06Callback, true) }
    println("R06_CALLBACK_ASSERT control=${control} stopped=${stopped}")
    if (control == 0 && stopped == 0) { 0 } else { 6 }
}
