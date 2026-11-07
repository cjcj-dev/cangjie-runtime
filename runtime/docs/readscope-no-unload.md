# Process-lifetime image retention

Native glibc Linux (excluding Android and OHOS) always retains Cangjie images.
Normal compilation and execution need no activation environment variable.
`CJ_READSCOPE_FAST` is no longer read: unset, `0`, `1`, other values and later
changes all have the same behavior. There is no per-runtime startup selection.
No public function signature, runtime parameter layout, calling convention or
TLS model changes.

ReadScopes maintain their thread-local depth and `AssertReaderActive` contract,
while bypassing reader slot/counter admission and release on this platform.
There is no per-reader fast/slow state. GC barriers and assertions remain active.

The platform choice follows the actual native implementation: `dladdr1`,
`dlinfo` and namespace-aware `dlmopen(..., RTLD_NOLOAD | RTLD_NODELETE)` on glibc.
Before publishing metadata, the loader verifies that the pinned native object
and namespace are exactly those owning the metadata address. Pin failure aborts
before publication. Direct `dlclose` cannot unmap the pinned image or invoke its
finalizers at close time. Native libraries without Cangjie metadata are unaffected.

`LoadCJLibrary` resolves the complete requested path through the native loader.
A basename match cannot bypass a missing-path error or satisfy a request for a
different image. Each successful load keeps its native reference, including
repeated loads. `UnloadCJLibrary` succeeds for a known handle but does not release
that reference, unregister metadata, remove static roots or unmap code. Null and
unknown names still fail. Reopening the same image preserves its native object,
namespace, symbol addresses and metadata; it does not reset plugin state.

This changes physical unload semantics: close is not resource reclamation.
Distinct plugins, their dependencies, metadata and static roots remain resident,
and repeated loads retain additional native references. Native plugin finalizers
and resources they release are deferred to process exit. Use explicit plugin
resource-release operations where provided; replacing/reloading a plugin image
requires a new process. Runtime finalization leaves the image catalog resident
and does not enable unloading. It still stops the managed runtime, so retained
metadata alone does not authorize managed calls after `FiniCJRuntime`.
`TypeInfoManager::Fini` still frees runtime-generated method tables and typeinfo
allocation arenas. Those generated objects do not acquire process lifetime merely
because the original image and its native metadata remain mapped.

Process exit invokes native finalizers. The runtime's fini callback requires a
verified pin for that registered image or completed runtime finalization. It
accepts never-initialized and initialized normal exit without purging metadata.
The unload writer retains its separate CHECK; an unexpected physical purge is
not silently accepted.

Compiler macro namespace handles already retain their native references;
runtime-managed macro handles call `UnloadCJLibrary`. Both now retain Cangjie
images by default on supported Linux. Pinning only the runtime SO would not
protect plugin metadata, so each metadata producer pins its own image.

Other platforms keep physical unload and the required admission, reader slots,
counters, pending-task checks, unlink/rendezvous/purge and metadata cleanup.
They lack this implementation's verified native namespace pinning; unconditional
NODELETE is not portable. These are platform capabilities, not debug or fallback
options. The physical path follows ZGC `zUnload.cpp:126-173` and
`zGeneration.cpp:1330-1388`; unlike ZGC's class-loader-owned storage, our image
storage is owned by the native loader and requires native retention as well.

`tests/run_readscope_no_unload.mjs SDK RUNTIME_LIB_DIR NEW_OUTPUT` uses the normal
installed compiler with no selector, then tests public load/close/reopen,
first-load errors, metadata after close, managed allocation, secondary namespaces
with a base decoy, both finalization orders and legal exit. The old environment
values are ignored controls with identical retention assertions. Native pin
failure is an intentional red at the real publication dependency, followed by
restoration and an independent non-Cangjie native-load control. Test output does
not by itself qualify a complete SDK, cross-platform behavior or performance.

## Public loading in glibc namespaces

`LoadCJLibrary` keeps GLOBAL loading in the base namespace. In a secondary
namespace it loads LOCAL into the current runtime's verified LMID. Use
`FindCJSymbol` for explicit lookup and ELF NEEDED dependencies between libraries;
an unrelated LOCAL library does not gain these symbols through RTLD_DEFAULT.

The loader identifies a private address in its own runtime image with `dladdr1`,
opens that image with NOLOAD, verifies the returned link_map identity, and only
then queries its LMID. A NULL dlopen handle alone does not prove the caller's
namespace. Lookup, identity, LMID or temporary-handle release failure returns a
load error; it never falls through to a secondary GLOBAL load. This detection
uses the same glibc-only platform boundary as native pinning. Other platform
loader implementations and the retention policy are unchanged.
