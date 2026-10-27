/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.core

// Called by the versioned macro reset entry in the throwing runtime. This is a
// managed call, not a C export: neither the caught object nor its layout crosses
// into the compiler's host runtime. The emitter obtains typeName from the target
// TypeInfo, roots the original exception, and keeps its mapping alive throughout
// this call. A nonzero status makes the emitter notify failure and preserve the
// original exception, rather than report a successful reset or lose its cause.
//  0: host copied/recorded the payload; -1: null callback; -2: missing type name;
// -3: payload preparation failed; -4: unknown throw; -5: host did not acknowledge.
func reportResetFailureV1(value: Any, typeName: CPointer<UInt8>, context: CPointer<Unit>,
    notify: CPointer<Unit>): Int32 {
    if (notify.isNull()) {
        return -1
    }
    if (typeName.isNull()) {
        return -2
    }
    var kind: UInt32 = 0
    var typeLen: UIntNative = 0
    var messageBytes = CPointer<UInt8>()
    var messageLen: UIntNative = 0
    try {
        try {
            var message = String.empty
            match (value) {
                case exception: Exception =>
                    kind = 1
                    message = exception.resetFailureMessageV1()
                case failure: Error =>
                    kind = 2
                    message = failure.resetFailureMessageV1()
                case _ => return -4
            }
            // TypeInfo owns the native, NUL-terminated name. Do not invoke a
            // virtual formatter or change its spelling to a display name.
            typeLen = unsafe { strlen(typeName) }
            if (typeLen == 0) {
                return -2
            }
            if (!message.isEmpty()) {
                // Copy every UTF-8 byte, including embedded NULs. This releases
                // the temporary GC pin before the callback; no GC storage is
                // lent to the host, and strlen must not measure this message.
                messageBytes = unsafe { LibC.mallocCString(message).getChars() }
                messageLen = UIntNative(message.size)
            }
        } catch (_: Exception) {
            return -3
        } catch (_: Error) {
            return -3
        }
        // The callback must not throw across the C ABI or retain these pointers.
        // Its ACK confirms copying/recording, not a successful macro reset.
        let acknowledged = unsafe {
            CFunc<(CPointer<Unit>, UInt32, CPointer<UInt8>, UIntNative, CPointer<UInt8>, UIntNative) -> Int32>(notify)(
                context, kind, typeName, typeLen, messageBytes, messageLen)
        }
        return if (acknowledged == 0) { 0 } else { -5 }
    } finally {
        unsafe { LibC.free(messageBytes) }
    }
}
