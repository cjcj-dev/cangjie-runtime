# Process-lifetime Cangjie image retention

Published Cangjie images, their code, and the TypeInfo, GenericDesc and method-table
storage they depend on remain resident until process exit on supported platforms.
This includes Linux/glibc, Windows x64, Apple, Android and OHOS; ARM32 remains
unsupported. There is no runtime selector for a physical-unload implementation.
`CJ_READSCOPE_FAST` is ignored, including changes after runtime initialization.

Ordinary metadata reads do not acquire an unload reader token. `ReadScope` is
trivially constructed and destroyed on these targets, including Debug/O0. The
reader assertion calls are discarded at compile time at their consumption sites.
There is no reader-depth TLS, reader atomic admission, lock or wait on this path.
This does not remove unrelated TLS, StableReadGuard table-version consistency,
type initialization/publication synchronization, GC barriers, roots, handshakes
or stack processing.

## Public close and reopen

`LoadCJLibrary` resolves the requested path using the native loader. A basename
match cannot satisfy a missing path or a request for a different image. Before
publishing metadata, registration must establish retention for the exact native
image and its dependencies in the actual loader namespace. Failure is an error
before publication; keeping the runtime library alone is insufficient.

`UnloadCJLibrary` logically closes a known handle. It does not release the retained
image, run unload-time finalizers, remove static roots or free published metadata.
Null and unknown names still fail. Reopening the same image preserves its native
identity, symbol addresses and metadata, without rerunning global initialization.
Native libraries that do not register Cangjie metadata retain their native API
behavior. A registration failure must not turn them into successful CJ images.

This deliberately changes physical-unload semantics. Close does not reclaim a
plugin's mappings, dependent metadata, static roots or associated native handles.
Applications must use explicit plugin resource-release APIs where available.
Same-path hot replacement and reclaiming old plugin images require a new process.
Repeated loads may retain additional native references; this policy gives no
bound on plugin count or virtual-address/memory use.

Normal process exit may run native finalizers according to the platform loader.
Runtime finalization does not release published metadata or reenable unloading.
It still stops managed execution: retained storage does not authorize managed
calls after `FiniCJRuntime`, and runtime reinitialization remains prohibited.
`TypeInfoManager::Fini` preserves published generated tables and allocation arenas
because image metadata can refer into them. Explicit published TypeInfo or
GenericDesc purge is rejected; unpublished preparation objects can still unwind.

## Failure identity and platform producers

A failed managed initializer keeps any already-retained storage and root
ownership, but subsequent load, initialization and symbol lookup fail. Its names
and lookup-cache entries are withdrawn without freeing published storage.
Retained bytes are not a permission to discover or execute failed initialization.
An independent metadata owner in the same image cannot republish the failed range.

On Windows, native attach success is a separate publication requirement. The
constructor registers an unpublished instance cookie. The paired, DLL-only
`__CJ_DllMainCRTStartupV1` calls the original CRT entry with unchanged arguments
and confirms that exact instance only after successful PROCESS_ATTACH. FALSE or
an exception without DETACH cannot gain this credential. Formal runtime Init or
public load publishes only individually confirmed instances after exact-image
retention and cookie validation. The executable entry is unchanged. Older DLLs
without this paired producer must be rebuilt; see
[Windows native registration v1](windows-native-registration-v1.md).

Linux/glibc verifies the owning link_map and namespace before using NODELETE.
Apple retains an actual native reference as well as preventing unmap, because
mapping retention alone does not prevent reference-count-zero finalization.
Android/OHOS registration uses the image-local, versioned native-retention callback
to open in the originating loader namespace and validates the resulting image;
a same-path object from another namespace is not a valid substitute. These are
loader-side operations, not extra work on metadata queries. Platform API names
alone are not evidence of lifecycle correctness.

## glibc namespaces

Public loading is GLOBAL in the base namespace and LOCAL in a verified secondary
namespace. Use `FindCJSymbol` or explicit ELF NEEDED dependencies for lookup;
unrelated LOCAL libraries do not acquire RTLD_DEFAULT visibility. The runtime
identifies its own image, opens it with NOLOAD, validates link_map identity and
then obtains its LMID. Detection failure returns an error rather than falling
through to secondary GLOBAL loading.

## Validation boundaries

Native platform builds and load/publish/query/close/reopen, failed initialization,
external native close and exit-finalizer checks are distinct obligations. The
runtime lifecycle fixture uses installed objects and public runtime APIs. It is
not a substitute for the paired installed compiler's ordinary DLL link path,
complete SDK validation or an address-reuse witness. Cross-built Android/OHOS or
iOS objects do not establish target execution. See the accompanying execution
report for fixed commits, actual native runs, failures and NOT_RUN items.

The ZGC reference separates unlink, rendezvous and purge (`zUnload.cpp` and
`ZGenerationOld::process_non_strong_references`). Its ClassUnloading-disabled
branch does not reclaim class metadata. CJ's native-image lifetime extension is
a deliberate platform difference: it is not a new ZGC reader mechanism, and does
not alter ZGC heap reclamation or table publication consistency.
