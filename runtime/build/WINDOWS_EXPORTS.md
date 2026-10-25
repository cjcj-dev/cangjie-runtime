# Windows export contract

The native link still uses `--export-all-symbols`. The checked contract covers
exports referenced by compiler emitters and the standard library, captured in
`src/windows_export_references.json`. Each entry has caller file/line evidence;
each input tree has a revision and a content digest. There is no prefix allowlist.

`generate_windows_exports.py collect` captures source references, excluding
comments. Emitter inputs contribute symbol strings and macro tables, plus
constant string concatenations resolved from their definitions. Source inputs
contribute identifiers, including foreign declarations. A complete Windows raw, the historical contract, and explicit C-linkage
runtime definitions jointly establish ownership. References owned by this DLL
must be present in the raw before collection can succeed; see the completeness
section below.

Collect from pinned compiler CodeGen/CHIR, LLVM lib/include, cjcj CodeGen and
stdlib/libs trees, using one `--consumer` per subtree:

```sh
python3 runtime/build/generate_windows_exports.py collect complete.raw.def \
  runtime/src/windows_export_references.json \
  --consumer emitter:compiler-codegen@COMMIT=COMPILER/src/CodeGen \
  --consumer emitter:compiler-reflection@COMMIT=COMPILER/include/cangjie/CHIR \
  --consumer emitter:llvm-lib@COMMIT=LLVM/llvm/lib \
  --consumer emitter:llvm-include@COMMIT=LLVM/llvm/include \
  --consumer emitter:cjcj-codegen@COMMIT=CJCJ/packages/codegen/src \
  --consumer source:stdlib@COMMIT=stdlib/libs \
  --runtime-source runtime@RUNTIME_COMMIT=runtime/src \
  --historical previous-registered.def \
  --native-sources stdlib=runtime/src/windows_stdlib_sources.json
python3 runtime/build/generate_windows_exports.py write complete.raw.def \
  runtime/src/windows_x86_64_exports.def --source-ref RUNTIME_COMMIT \
  --historical previous-registered.def
```

Replace each `COMMIT` with the source tree's 40-digit revision. The initial
capture uses the complete registered definition from runtime commit
`d2840f7621dfe6d31e260e9bedd1dc33c8b08a5f`; this is a migration input, not the
candidate DLL under test. Capture is an explicit contract update: do not recapture
from a failing candidate to make a missing symbol disappear. Review source
changes that construct names dynamically when updating compiler consumers.

Generation and verification use the same reference selector. Generation fails
if any captured symbol is absent. Verification also requires the registered
`.def` to contain every caller requirement and captured historical entry. Then
it checks all registered names, ordinals and DATA attributes in the candidate.
Extra implementation symbols may remain. The `.def` remains usable by existing
readers and is not itself a linker input; ordinal drift is rejected, not repaired
by inventing a new numbering.

Run `runtime/tests/windows_exports/test_contract.py --raw complete.raw.def` to
exercise the actual CLI, both libc++ ABI tags, and removal of each captured
symbol. The separately owned Windows consumer export contract workflow runs the normal
MINGW64 product build, saving toolchain identity, raw/registered definitions,
reference provenance, DLL hashes and results.

Native std references must come from the Windows build's translation units.
Scanning every `.c` file also admits Linux/Unix implementations that Windows
never compiles. `.cj` sources and compiler emitter collection are unchanged;
native headers are not independently treated as translation units.

Configure the pinned std tree for Windows. When the complete std configure is
blocked by unrelated dependency builds, `export_source_config` configures **all**
`libs/**/native/CMakeLists.txt` with the real CMake target selection. It does not
build or link std, generate a replacement source list, or select by filename.
It needs the compiler SDK headers/schema/flatc for the native AST configuration.
For example, on a Linux host with MinGW cross compilers:

```sh
cmake -S runtime/build/export_source_config -B native-windows \
  -DSTDLIB_SOURCE=/absolute/pinned/stdlib \
  -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
  -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
  -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++
python3 runtime/build/generate_windows_exports.py capture-native \
  native-windows /absolute/pinned/stdlib/libs \
  runtime/src/windows_stdlib_sources.json
```

Pass `--native-sources stdlib=runtime/src/windows_stdlib_sources.json` to the
`collect` command above. The manifest records the configured platform, every
native source directory's CMake hash, and the portable translation-unit paths
from `compile_commands.json`. Capture rejects a configuration missing an entire
native directory; collect rejects missing manifests, non-Windows selection,
changed native CMake inputs, and nonexistent/out-of-tree selected sources.
Source-content and manifest hashes are retained in the consumer reference file.
Neither collection nor checking infers the expected set from a failing DLL.
Regenerate both JSON and `.def` through the CLI; do not edit symbol entries.

## Fixed-source completeness and historical compatibility

The current collector requires `--runtime-source LABEL@40_DIGIT_SHA=DIRECTORY`
(the runtime `src` tree) and accepts `--historical previous-registered.def`.
It resolves caller ownership against the union of the complete raw, the historical
contract, and explicit C-linkage function definitions in that fixed runtime tree.
This catches new Reference services even when an older raw omits them. It does
not infer a universe from a runtime-name prefix. The source scan is conservative
ownership evidence; it is not a Windows compiler/preprocessor or a replacement
for complete linker output (in particular, asm aliases and C++ mangling).

Use `--diagnostics missing-and-provenance.json` to retain the exact caller and
runtime-definition anchors when collection fails. A missing required symbol is
an error; failed collection leaves the output references file untouched. The
historical symbols remain required even if the new callers no longer use them.
Generated references capture the historical names, ordinals and DATA flags.
`write --historical previous-registered.def` refuses missing exports, changes to
historical ordinals/DATA, or a historical input that lost captured entries.
`check` checks the whole registered contract, including unused historical names.
These stricter ordinal rules supersede the earlier name-only checker behavior.
They do not authorize inventing ordinals or rewriting a failing DLL's contract.

The Windows post-link command now preserves the linked DLL and complete raw in
`CMakebuild/windows-export-link/` **before** checking or publishing the product.
It requires `llvm-readobj` (or `-DWINDOWS_EXPORT_READOBJ=/path/to/llvm-readobj`).
`check --dll linked.dll --readobj llvm-readobj` reads the complete actual PE export
table and checks its names/ordinals against the raw, then checks historical and
consumer requirements. DATA remains checked against the linker's DATA flags;
this does not infer C++ types from export addresses. Tool failure or table mismatch
is a failure, not a reason to shrink the contract. CI must save the normal link
log, check result, these preserved artifacts, toolchain identity and any install
results separately. A saved DLL from a rejected gate is not a qualified install.

Stage inputs: R65ab0659d0e657c92a9ddabe8a10f8bc22dd2eef;
C8ba3eb11c18527d5641794225b901202071d726f consumer **source only**;
Nde7f37458b456c4a7ef1ebe2726d81fcfd9904c2 consumer **source only**.
Collect CodeGen/CHIR/mangle and LLVM lib/include plus the compiler macro runtime
loader separately labelled; std native sources must use the actual Windows TU
selection. Recheck changed callers when a later C fix freezes; never label an
existing ELF with a newer parser/source commit.

`CJ_MCC_NewFinalizer` is still a historical registered export, while the current
Reference mechanism registers at successful root construction instead of object
allocation. The new generator preserves and reports this conflict. It neither
removes that historical obligation nor reinstates a retired allocation fallback.
The runtime/CI owners must carry the concrete raw result to the controller for
an explicit compatibility decision. Linux `nm` and collector fixtures do not
satisfy Windows DLL acceptance.

The added `node runtime/tests/windows_exports/test_completeness.mjs EVIDENCE`
exercises incomplete raw rejection, five runtime-defined services, independent
controls, historical retention, DATA/ordinal changes and the DLL-table adapter.
Its synthetic tables prove only the generator behavior; normal Windows production,
actual DLL checking, installation and package/unpack remain separate gates.
