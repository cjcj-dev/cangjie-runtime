# Normal runtime build and consumers

CMake writes libraries, executables and generated CJThread headers beneath
`BUILD/runtime-staging`. `cmake --install BUILD --prefix INSTALL` installs the
runtime and boundscheck pair through the normal platform install rules. No
hash-named source output, inventory scan or publication target is involved.

For internal C++ consumers of an existing build/install, select the actual
library directory with `GCV2_RUNTIME_LIB_DIR`, the matching generated-header
root with `GCV2_RUNTIME_OUTPUT_ROOT=BUILD/runtime-staging`, and the normal CMake
build directory with `GCV2_RUNTIME_BUILD_DIR=BUILD`. The latter supplies
`compile_commands.json` so tests use the product's data-layout macros. These
paths are explicit inputs, not a certificate of a copied library's origin.
On AArch64, an external internal-test build additionally supplies
`GCV2_RUNTIME_COPY_OBJECT` from that same build's RuntimeCopy target. In-tree
CMake tests consume the target object directly.

The post-link GC gate consumes that build-tree pair directly. Its normal mode
runs C++ and language consumers. `GC_UNIT_GATE_LANGUAGE_TESTS=defer` records
language NOT_RUN/deferred while running C++; `only` runs language consumers.
`GC_UNIT_GATE_SKIP=1` records NOT_RUN. Each invocation keeps its own logs and
actual runner exit codes. There is no cached PASS or product-copy step.

Language inputs are documented in `tests/gc_unit/LANGUAGE_TOOLCHAIN.md`.
The A2 fixture requires zx and an explicit same-version CJ linker via
`MANAGED_METADATA_LINKER`; a host C++ compiler does not supply that ABI.

The isolated `run_young_weak_product_cut.sh` compatibility entry forwards to
zx. Set `GCV2_RUNTIME_BUILD_DIR` and `GCV2_RUNTIME_LIB_DIR` to the actual selected
CMake build and target output directory. It rebuilds the archive and real SO
through CMake, retains each executed pair as evidence, and restores the cut.
It never selects a hash publication. Its companion regression checks the
exact changed failure set, unchanged test ELF and restored product bytes.
