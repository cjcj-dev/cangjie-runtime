# OHOS AArch64 execution arm

This arm runs the OHOS product SO under qemu-aarch64 with an OHOS musl loader.
It is separate from `MRT_GC_UNIT_OHOS_HOST`: it never compiles the host stubs.
It does not certify a physical device, a system image, ArkVM or device services.
The SDK's NDK `libc.so` is a link-time stub, not an executable loader.

On the build host, supply a private output directory and the public native SDK:

```sh
export OHOS_PUBLIC_SDK=/path/to/ohos-sdk/linux/native
export OHOS_DEVICE_BUILD=/private/build
bash runtime/tests/gc_unit/ohos_device/build.sh
export OHOS_RUNTIME_LIB=$OHOS_DEVICE_BUILD/runtime-staging/lib/aarch64_Release
export OHOS_LOADER=/private/ohos-musl/lib/libc.so
export OHOS_DEVICE_OUT=/private/results
bash runtime/tests/gc_unit/ohos_device/run.sh
```

The verified loader input was built from OpenHarmony-v5.0.3-Release
`third_party_musl` commit `eed5dab29bc7223e49ffc247f7f2ee0ce94127ec`, using
that SDK's clang 15.0.4. Its SHA256 is
`4b901129350a146000b349e04ad47481f0a3ea786da9253d77e3a3e48aabd620`.
It is an explicitly supplied, rebuilt loader, not extracted from an image.
The build used `scripts/porting.sh -p linux`, `configure --target=aarch64
--syslibdir=/lib`, `--target=aarch64-linux-ohos -fuse-ld=lld`, SDK compiler-rt
builtins, and `-DFEATURE_PTHREAD_CANCEL -DCXA_THREAD_USE_TSD`. It did not define
`MUSL_AARCH64_ARCH`; `sys/queue.h` came from the same SDK sysroot.

`run.sh` copies its libraries into an independent execution root and records
ELF/SO/loader SHA256, architecture/interpreter, dependencies, affinity, uptime,
wall time and separate `empty.rc` / `cycle.rc`. Set `OHOS_DEVICE_ELF` to reuse the
same compiled probe across controlled product changes. Run through the local
build/test-slot orchestration and reserve a measurement CPU domain.

The native fixture attaches through `MRT_NewForeignCJThread`. `RunCJTask`
would present an ordinary C++ frame as a managed frame without compiler
function metadata, which is not a valid AArch64 pointer-authentication fixture.
The input is a real export -> foreign proxy -> context -> handler object graph;
only the existing major-GC entry produces its cycle work map. The registered
platform event callback receives the product resolver task, which the fixture
executes after GC has returned to idle. The native handler checks its actual
arguments and the product's retained-root result. The final assertion also
checks that the resolver changed the export object's active state. No private
map seeding, copied resolver, test hook or host implementation is involved.

AArch64 `ResolveCycleRefStub` / `CJ_MCC_N2CStub` preserve the callee in x0 and
the owner/proxy in x1/x2; the native observer follows that ABI. This native
observer does not qualify compiler-emitted managed handler bodies.

Two independent target cases are always run: `empty` (no initialization or
work) and `cycle` (major GC, post, consumer, handler and state). Target failures
return 1 after printing all final values; setup errors have distinct codes.
The accompanying delivery evidence cuts both `ZGenerationOld`'s post call and
`ZCrossVM`'s resolver-stub call: each must make `cycle` fail at its target
assertion while preserving `empty`.

ZGC anchors for the sequencing invariant are `zGeneration.cpp:1361-1372`
(publish non-strong work after unblocking resurrection) and
`zDriverPort.cpp:127-148` (publication and actual consumption are distinct).
OHOS interoperation and its public SDK have no ZGC counterpart.
