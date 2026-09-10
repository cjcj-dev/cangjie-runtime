# Runtime configuration outputs

A single-config CMake build writes intermediate products below
`runtime/output/temp/<CANGJIE_RUNTIME_CONFIG_ID>/`. Read the ID from that
build's `CMakeCache.txt`; there is no shared “latest” directory.

```sh
id=$(sed -n 's/^CANGJIE_RUNTIME_CONFIG_ID:INTERNAL=//p' BUILD/CMakeCache.txt)
lib=$(bash runtime/build/resolve_runtime_output.sh runtime "$id")
sha256sum "$lib/libcangjie-runtime.so" "$lib/libboundscheck.so"
GCV2_RUNTIME_CONFIG="$id" bash runtime/tests/gc_unit/gate_gc_unit.sh
```

The manifest binds the ID to its library directory and full input signature.
`runtime-build-inputs.txt` records the exact signature input; its SHA-256 equals
`CONFIG_SIGNATURE_SHA256`. The gate records both selected SO hashes. Packaging
and test drivers should retain these hashes with the build's source provenance.

Identity is computed before the first CJThread/runtime output. It includes all
effective cache values, ordinary variables present after toolchain loading, and
variables added or changed by product configuration. A cache entry's type or
help text does not determine whether it affects the product, and an ordinary
variable may override its value. Product option names are not enumerated.

Compiler detection runs between two scope snapshots. Unchanged variables
introduced solely during that phase are mechanically identified, rather than
classified by their names. CMake's persisted compiler state files are hashed
instead, along with the toolchain file and directory compile/link properties.
The build directory's `runtime-compiler-probe-temporaries.txt` records that
mechanically selected set for diagnosis. This avoids incorporating compiler
probe scratch values that exist only on first configure.

The remaining exclusions are generated output paths/identity, cache format and
GUI metadata, regex scratch state, completed compile-probe results, and the
system/install/bootstrap scratch values listed in `RuntimeOutputLayout.cmake`.
The resulting compiler paths, flags, install prefix and product inputs remain
included. Extra inputs may conservatively produce different IDs; a product
option need not be added to an identity allowlist when it is introduced.

Multi-config generators are rejected at configure time. Tests can run the
lightweight CMake regression or exercise the real runtime build:

```sh
cmake -DTEST_RUNTIME_SOURCE_DIR="$PWD/runtime" -P runtime/tests/test_runtime_output_layout.cmake
python3 runtime/tests/test_runtime_output_product.py \
  --source runtime --work /absolute/isolated/work --mode environment --build
```

The product regression checks repeated configure, OFF/ON/OFF identity,
unchanged earlier products, selected SO hashes, and restored bytes. Its
`toolchain`, `environment`, `initial-cache`, and `arbitrary` modes distinguish
file contents, effective ordinary values, typed cache values, and newly named
inputs connected to actual product compilation. Run these on the build host.
