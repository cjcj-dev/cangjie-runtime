# Native OHOS-shaped host arm

`MRT_GC_UNIT_OHOS_HOST=ON` requires native x86_64 Linux. Before configuring,
provide the host loader shims through `LD_LIBRARY_PATH`. For example, copy the
host compiler's `libc.so.6` to an isolated directory under the name `libc.so`:

```sh
mkdir -p "$PWD/host-shims"
cp "$(clang++ -print-file-name=libc.so.6)" "$PWD/host-shims/libc.so"
LD_LIBRARY_PATH="$PWD/host-shims" cmake -S runtime -B build-ohos-host \
  -DMRT_GC_UNIT_OHOS_HOST=ON -DMRT_TESTABLE_INTERNALS=ON \
  -DCJ_SDK_VERSION=0.0.1 -DCMAKE_AR_PATH=ar
```

Configure extracts the mandatory startup library selection from
`SignalStack.cpp:FindSymbolInLibc` and runs a native `dlopen` probe. An unavailable
or unloadable library fails configuration with `OHOS_HOST_LIBRARY_MISSING` and
its name. Success is rechecked on each configure, including reuse of an existing
build directory. Optional ArkTS/ROM services and application-selected libraries
are not startup shim requirements. This check concerns loader availability, not
the semantics of optional services or the full OHOS device ABI.

The standalone runner still prepares its own isolated execution directory and
selects the OHOS arm from the published product configuration. Configure does
not change that dispatch or install libraries into the host system.

Run the regression on the host using:

```sh
python3 runtime/tests/gc_unit/test_ohos_host_libraries.py \
  --source "$PWD" --output "$PWD/configure-test"
```

It configures the real runtime with a complete shim directory, removes one
library, reconfigures the same build directory, restores the library, and checks
the default arm without shims. Separate output directories are required for
concurrent runs.
