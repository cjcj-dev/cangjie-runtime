/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.sync

/* MultiConditionMonitor-related intrinsics*/
@Intrinsic
func multiConditionMonitorWait(multiConditionMonitor: MultiConditionMonitor, obj: Object, timeout: Int64): Bool

@Intrinsic
func multiConditionMonitorNotify(multiConditionMonitor: MultiConditionMonitor, obj: Object): Unit

@Intrinsic
func multiConditionMonitorNotifyAll(multiConditionMonitor: MultiConditionMonitor, obj: Object): Unit

/* Waitqueue-related intrinsics*/
@Intrinsic
func waitQueueInit(obj: WaitQueue): Bool
