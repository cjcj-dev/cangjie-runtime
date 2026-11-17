# Native image registration ABI v1 (paired producer/consumer hold)

This is a staged producer/consumer contract for Android and OHOS x86_64 and
AArch64. ARM32 remains unsupported. It does not change CJFileHeader, TypeInfo,
GC layouts or the user-facing Cangjie loader API. It requires rebuilding the
startup object and its consuming CJ images together with the runtime; neither
side qualifies for independent deployment. The controller must record this pair
hold before integration. Native target execution is not yet verified.

The C entry is `MRT_LibraryOnLoadRetainedV1(uint64_t metadata, bool enableGC,
void* (*imageRetain)(const char* path, int flags))`. Version 1 is encoded in the
entry and exported callback symbol, not selected by an environment variable.
The first two arguments retain the original registration entry ABI. The third
is the native C calling-convention function pointer to this image's protected,
exported `__CJ_ImageRetainV1`. There are no optional fields, structure packing,
ownership transfers or persistent callback registration. Its result is a native
`dlopen` reference, released by runtime after exact-image verification and
NODELETE promotion. Assembly preserves the return address and really calls
`dlopen`; it must not tail-jump, since Bionic/OHOS selects the caller namespace.

Before any image catalog or TypeInfo publication, runtime proves the callback
and metadata belong to the same native image. A NOLOAD lookup made by that
callback must resolve its exported callback to the exact pointer supplied by
Cjstart. Only then can a second image-local NOLOAD|NODELETE promotion run; its
result must resolve the same pointer before temporary references are released.
Callbacks are not called under runtime catalog/registration locks. They do not
initialize packages or perform query-side registration. Table-version and GC
synchronization are unchanged.

The main executable is process-owned, identified by its actual PT_PHDR/AT_PHDR
relationship, not its filename. An image with its own loaded DF_1_NODELETE can
also be accepted without a path lookup: the native loader read that flag before
calling initializers. This handles exact native images even when Android force
loads duplicate files. A matching pathname or namespace alone is insufficient.
For a duplicate lacking NODELETE, a callback that resolves another copy is
rejected before metadata publication. Supporting every such historical input
still requires the normal final-ELF producer to set NODELETE, or another proven
native exact-instance retention facility; that cross-repository producer change
has been requested and has not been implemented by this runtime task.

The original `MRT_LibraryOnLoad` stays exported. Native glibc Linux retains its
existing exact link_map/LMID implementation. On the new Android/OHOS policy an
old image is accepted only when its actual lifetime is independently proven
(main executable or own NODELETE flag); otherwise it fails before publication
with an explicit requirement to rebuild its startup object/image. Existing
components must be rebuilt through their normal production chain, not relabeled
as v1-compatible. The new Cjstart cannot run against an old runtime lacking the
v1 symbol. This is an explicit paired ABI hold, not silent binary compatibility
or permission to drop a supported target.

## Apple native lifecycle (separate from the ELF v1 ABI)

macOS/iOS registration uses the existing Section.cpp `_CJMetadataStart` exported
slot as an exact-image witness: the slot's native owner and its value must both
match the incoming metadata. RTLD_FIRST lookup prevents a dependency's slot
from qualifying the root image. The main executable is checked against dyld's
actual executable header, not a path. No new Apple startup ABI is introduced.

A NOLOAD lookup is used only to prove the already loaded identity. Promotion
uses a second call without NOLOAD, because dyld's early NOLOAD return precedes
late NODELETE handling. The promoted native reference is process-owned and
never released: NODELETE mapping retention by itself does not prove absence of
close-time terminators or preservation of the registered dependency closure.
The temporary lookup reference is released after exact-instance verification.
Only registration bears this cost. These are staged implementation properties;
macOS/iOS native build, constructor reentry, close/reopen and exit-fini evidence
remain NOT_RUN until the official native runner validates them. Existing images
must expose their actual Section.cpp witness; no filename-only fallback is
accepted. Normal old/new image support must be checked by that native run.
