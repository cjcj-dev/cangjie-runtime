# Paired macro reset failure transport

The target `std.core` entry is an ordinary managed function, retained and called
through the compiler's implicit-use/CJO/CHIR machinery:

```cangjie
func reportResetFailureV1(value: Any, typeName: CPointer<UInt8>,
    context: CPointer<Unit>, notify: CPointer<Unit>): Int32
```

The callback uses the target C calling convention:

```c
int32_t notify(void *context, uint32_t kind,
    const uint8_t *type, size_t typeLen,
    const uint8_t *message, size_t messageLen);
```

The emitter obtains `typeName` from the caught object's target-runtime TypeInfo
using the existing runtime services. It roots the original exception and retains
the target image and callback context until the task finishes. The helper does
not format the runtime name, invoke a virtual property, or inspect a foreign
runtime's managed object. Exception/Error access their own private `detailMessage`
through non-open internal methods; their layouts are unchanged.

Kinds 1 and 2 carry the original Exception and Error respectively. The type is
the runtime's raw UTF-8 name, not a reflection display name. Message length is
`String.size`, excluding the terminator and including any embedded NUL. An empty
message is represented by null/zero. Nonempty bytes are copied into target-native
storage with the existing `LibC.mallocCString`; its GC pin ends before the callback.
The host copies synchronously and must not retain pointers or throw across the C
ABI. The helper frees its buffer in the target runtime after the callback returns.

| Helper result | Meaning | Emitter responsibility |
| --- | --- | --- |
| 0 | Host acknowledged copying/recording the failure | Finish the failed reset task; do not send kind 0 |
| -1 | Null callback | Reject notification, preserve original exception |
| -2 | Null or empty runtime type name | Report explicit export failure, preserve original exception |
| -3 | Payload preparation threw | Report export failure, not the secondary exception |
| -4 | Value is neither Exception nor Error | Report unknown throw without inventing a message |
| -5 | Host returned nonzero | Preserve notification failure and original exception |

The emitter owns the kind 3/4 failure notification and original-exception
diagnostic path for nonzero results. It must also handle an exception escaping
before the helper can return, including stack exhaustion. Kind 0 is reserved for
a normally completed reset. An ACK is not proof that a reset succeeded.

The new reset entry uses the paired compiler's `ir2` special name. Old `ir`
macros require normal recompilation or explicit rejection. The unchanged size of
the two-pointer notify/context structure does not make the old callback ABI
compatible. Do not guess the helper's mangled name or call it as a C function.

## Required paired validation

All cases must use fixed reviewed C/R/std inputs and the normal macro definition,
load, reset, notification, GetTaskRet, ReleaseHandle, owned-runtime Fini, and host
exit chain. These are requirements, not claims of execution.

- Success control: kind 0, macro body and ordinary dependency execute, host exits.
- Exception: derived type overrides `message`, `getClassName`, and `toString` to
  throw or return different text. H must receive the actual runtime type and the
  private constructor message, including UTF-8 and embedded NUL; body is not run.
- Empty Exception message: kind 1 and zero length remains a valid failure payload.
- Error: use a legally produced target Error, not an external call to an internal
  constructor or a host Error masquerading as a target object. Apply the same
  payload and no-override assertions.
- Unknown value and missing type: exact status and explicit emitter failure;
  neither can become a successful empty-message notification.
- Preparation failure and nonzero callback ACK: retain the primary error, publish
  failure, release temporary native storage and complete the task/owner teardown.
- Cut the actual message accessor/consumer edge in an isolated candidate, require
  the original-message assertion to fail while the success control still passes,
  then restore and rebuild the affected normal target.
- Borrowed host-runtime control must remain usable; only an owned independent
  runtime is finalized. Rebuild affected macros and reject stale `ir` inputs.

Direct helper tests can establish status or byte semantics only. They cannot
replace the paired emitter, H callback, namespace, lifecycle or package tests.
