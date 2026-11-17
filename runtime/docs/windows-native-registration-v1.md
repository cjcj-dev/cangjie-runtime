# Windows native registration v1

This producer/consumer pair is required before CJ metadata from a DLL becomes
public. It does not change TypeInfo layout, the GC ABI or table-version guards.
It is a loader registration ABI and must be integrated with the compiler's DLL
link entry and rebuilt Windows images as one change.

* `cjstart.o` calls `MRT_LibraryOnLoadPendingV1(metadata, enableGC, &cookie)`
  from its CRT constructor. `__CJ_NativeRegistrationV1` is an image-local,
  pointer-aligned writable slot. The runtime assigns a distinct, never recycled
  opaque cookie for each constructor invocation. A pending registration retains
  native storage, but does not create an address-directory entry, BaseFile,
  package, TypeInfo, GenericDesc or static-root registration.
* DLL output additionally links the normally installed `cjstartdllentry.o`,
  with entry `__CJ_DllMainCRTStartupV1`. This entry forwards unchanged arguments
  to the existing `DllMainCRTStartup`, including CRT constructors, the user's
  DllMain, notifications and return value. Only a normal TRUE return for
  PROCESS_ATTACH calls `MRT_LibraryNativeAttachV1(cookie, true)`. FALSE never
  grants publication; an exception escaping the original entry never reaches
  the completion call. Completion records readiness, not publication.
* CRT fini calls `MRT_LibraryNativeDetachV1(cookie)`. Unpublished pending/ready
  registrations become permanently failed. Repeated completion cannot revive
  them. Published storage remains process-owned, including normal exit fini.
* Formal RuntimeInit, or the public LoadCJLibrary path after native load
  success, commits individually confirmed registrations. It pins the exact
  metadata owner outside the constructor/catalog locks and checks the current
  image-local cookie. Base address, pathname and mapped bytes alone are not
  evidence of successful native initialization. A stale callback/cookie cannot
  name a later mapped instance, even if its base or metadata address is reused.
* The main executable does not use a DLL CRT entry. Its metadata can be
  committed by formal RuntimeInit, the normal post-native-startup API boundary.
  RuntimeInit and public CJ loading are not supported from DllMain/native CRT
  constructors (they initialize threads and managed runtime state).

Old DLLs using only `MRT_LibraryOnLoad` have no native completion proof. The new
runtime leaves those registrations unpublished and rejects their public CJ load
instead of treating a retained failed handle as successful. Official Windows
CJ DLL producers and dependencies must be rebuilt with the paired startup and
compiler entry; this is not a claim of transparent old-image compatibility.
Non-CJ native libraries have no CJ registration and retain their existing load
behavior. No per-query synchronization is added.

A retained failed native image may consume process-lifetime virtual memory. It
must stay undiscoverable after RuntimeInit and must never be revived by reopening
the same native handle. A different successfully initialized instance has its own
cookie and may publish independently. Mapping retention and failed-identity
withdrawal are separate properties.

The runtime-only fixture links the same entry assembly and uses real CRT/DllMain
failure or exception before formal Init, then tests the exported reflection
package lookup and an independent successful image. This is finite native
coverage, not a full Windows SDK or all image/dependency combinations. Address
reuse is covered only when actually observed; lack of reuse is not ABA evidence.
