# Runtime configuration outputs

Runtime and boundscheck link into `BUILD/runtime-staging`, private to the CMake
build tree. After both libraries exist, the product POST_BUILD step publishes an
immutable pair below `runtime/output/temp/<CANGJIE_RUNTIME_CONFIG_ID>/` and runs
the GC gate against that published pair. Multi-config generators are rejected.
CMake 3.19 or newer is required for generation-time target input exports.

The final ID exists **after build**, not at configure time. The cache records the
last successfully published ID; reconfiguring does not publish a new product.
This replaces the old configure-time ID contract: generator expressions can
read custom target properties which are only resolved during generation.

```sh
cmake --build BUILD --parallel 16
id=$(sed -n 's/^CANGJIE_RUNTIME_CONFIG_ID:INTERNAL=//p' BUILD/CMakeCache.txt)
lib=$(bash runtime/build/resolve_runtime_output.sh runtime "$id")
sha256sum "$lib/libcangjie-runtime.so" "$lib/libboundscheck.so"
GCV2_RUNTIME_CONFIG="$id" bash runtime/tests/gc_unit/gate_gc_unit.sh
# Re-publish/check the same linked pair without requiring a new link:
cmake --build BUILD --target publish_runtime_output
```

Identity is derived from generated `compile_commands.json`, generation-time
exports of evaluated compiler/linker inputs for every compiled target, the native
generated link commands (Makefile link.txt or Ninja's command export),
persisted compiler state, toolchain contents, and CJThread's actual headers and
archives. It does not classify variables or collect a whitelist of arbitrary
target properties. Private build/staging paths and CMake process-local directory
markers are normalized. Linked library hashes additionally bind the signature
to the product bytes and embedded source revision. A source change can therefore
produce a new publication even when compiler options are unchanged.

The publisher hashes inputs and both SOs, copies the pair and CJThread inputs to
a temporary sibling directory, verifies the copies, then atomically renames the
directory. An existing ID is accepted only if all owned files are byte-identical;
it is never overwritten with different contents. The manifest, signature input
JSON and product hash inventory travel with the publication. Gate status files
are observations and may be updated by later gate executions. The receipt is
also mirrored beside the linker SO for the existing outer build gate; the
product gate itself always executes against the published directory.

CJThread configures early because runtime needs its generated headers. Both its
nested build directory and intermediate outputs are private to the parent build
tree. Packaging continues to install from CMake targets. Consumers must select
an explicit published configuration; there is no mutable “latest” alias.

The GC gate and standalone runner also accept a directory containing only a
copied runtime/boundscheck pair, as supplied by the composition gate. With no
explicit `GCV2_RUNTIME_OUTPUT_ROOT`, they locate published headers by **both** SO
hashes and verify the complete header inventory before compiling. Multiple
matching publications are accepted only when their header bytes agree. External
builds without a local publication must supply `GCV2_RUNTIME_OUTPUT_ROOT`.
The library selection itself remains the caller's explicitly supplied pair.

`tests/test_runtime_copied_headers.py --runtime runtime --publication <root>
--work <isolated-work>` runs the real standalone AST entry with a copied pair,
records the actual compiler arguments, and compares consumed header hashes to
the publication inventory. A compatible stale-header control permits AST
compilation to finish so the identity assertion can detect a wrong consumer.
Add `--entry gate` to exercise the GC gate's selection and complete C++ suite.

Run the regression on the build host (its work directory must be outside source):

```sh
python3 runtime/tests/test_runtime_output_product.py \
  --source runtime --work /absolute/isolated/work --mode override --build
```

The `toolchain`, `environment`, `initial-cache`, `override`, `arbitrary`, and
`indirect` modes check OFF/ON/OFF input changes through the real CMake entry,
compiled product results, repeat publication, earlier-directory preservation,
resolver selection/hashes, and restored bytes. `indirect` changes only an
ordinary custom target property read via a generator expression; no new identity
option name is registered for it.

The manual `run_young_weak_product_cut.sh` fault-injection runner bypasses the
POST_BUILD step when it executes `link.txt`. It therefore retains the just-linked
runtime and boundscheck paths from `BUILD/runtime-publish-args.txt`, rather than
resolving a prior publication. Each cut/restoration snapshot records source path,
link timestamp and SHA-256 in `linked-product.json`; tests load that snapshot.
In an isolated Unix Makefiles checkout with `runtime/CMakebuild` configured and
`cj_gc_unit` built with both test options enabled, verify the serial cut using:

```sh
python3 runtime/tests/gc_unit/test_young_weak_cut_identity.py \
  --runtime runtime --elf /absolute/path/to/cj_gc_unit \
  --evidence /absolute/new/evidence-directory
```

This checks the same ELF against baseline/cut/restored pairs, requires exactly
the serial target to change result, and checks restored bytes and source identity.
