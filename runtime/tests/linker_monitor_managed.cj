// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
foreign func R05Check(control: Bool): Int64

@C
func R05UserMonitorScope(): Int64 { 5 }
@C
func R05PlainScope(): Int64 { 7 }

main(args: Array<String>): Int64 {
    let rc = unsafe { R05Check(args.size > 0) }
    println("R05_MANAGED_ASSERT rc=${rc}")
    rc
}
