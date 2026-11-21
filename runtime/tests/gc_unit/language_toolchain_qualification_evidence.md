# Current pinned tuple qualification evidence

Qualification was measured on kkk2 for the 853e native-producer SDK package. No runtime
product code, std source or compiler change was made. The record replaces the previous
`f2283db793…` stage1 tuple: with that SDK's std and the 853e target runtime the managed
finalizer fixture reported `finalized=0` of 64 (gate run `run.ZgGVELBNlZ`, 2026-11-20),
so that tuple no longer qualifies the current AOT ABI pairing.

The package is the archive produced by `node build/cli.mjs sdk --archive-inputs …` (cjcj
`6a26bcde09edfa65d4e5eba801c4cbb01a98897f`) from the 853e native producer install:
`archive.tar.gz` SHA256 `fe17f688c60545f715bad0aac1401a13c8bf89286fba489a02ee52aa8159d86d`.
It was unpacked once into a fresh directory and every measurement, consumer and gate run
below used that unpacked tree, not the producer's install tree. Inputs recorded by the
producer: compiler 6a26bcde, LLVM `74bb3a7186e171faf863f9c281ba9773e584bb64`, static std
built by the B2 stage2 compiler from runtime/stdlib `54497690da71724010cd0d65b731ebc4f941fe52`
(no `stdlib/libs` source difference to 853e), target runtime `853e939928ecdb17eef38409d137268dce15cc0d`.

The SDK compiler entry is the cjcj native launcher. `bin/cjc` is an alias of the
launcher `bin/cjcj-stage1`, whose `bin/cjcj-stage1.entry` names the entity
`libexec/compiler/cjcj-stage1` and the compiler-execution directories under
`.compiler-exec/`. The launcher stub is shared by cjc/llc/opt, so admission binds each
`.entry` and its entity in addition to the launcher.

| Measured language component | SHA256 |
|---|---|
| cjc / llc / opt launcher | `420ac6fc43d8b8029c3f91fbaae50f6eb5af29ef90792b35c2f249e41249c0bf` |
| cjc entity (`libexec/compiler/cjcj-stage1`) | `3be32b480581efb2c9e40708551c4cac1f13a08113104e1c5007f9e99b01138c` |
| llc entity | `18407da25edbfedced94e74b0cb0cd232bda23e9ba170b85a21d988e8bad2966` |
| opt entity | `a83a79f29ea1de27802f3bee9c97f2af25ff50c47c44d0509762b12c0119ecc8` |
| std (archive set) | `d2270fa702b9bcfe7883173383a5f3da2b6f6812e4fa011fcc33769af98b9c3f` |
| std_core | `6484711bfc827990534249f65e2ee5f2c7979e4df284c4b92f88ad3814f50b98` |
| runtime | `4d6e503d7285ba39e5919d6419cb94e8d0d244b099005c6a33f760a62f9fed9b` |
| compiler host runtime (`.compiler-exec/runtime/lib/linux_x86_64_cjnative`) | `c457d8add4f4f0fa6d8bffaf601840c6ece676b814efd6efb0faa625b1bb5b1c` |

The compiler host is the package's own `.compiler-exec` set (three directories named by the
launcher entry); its complete file-set identity is `compiler_host_set` in the record. The
colour checker (`cjcj/ci/bootstrap/std_runtime_colour.py`, `f790d583…`) and the declared
official reference runtime (nightly-1.3.0-alpha.20260925001050, `a656fbc7…`) are unchanged
from the previous record. The checker on target runtime `4d6e503d…`, that reference and
std-core `6484711b…` returned rc 0 / stdout `1`.

Producer/consumer observations for this tuple:

- The measured std-core `core.o` (`8fd3e7c6…`) defines `CJ_MCC_NewObjectFast` at `0x98570`:
  `49 8b 17; 48 8b 02; 48 8b 4a 08` (`mov (%r15),%rdx; mov (%rdx),%rax; mov 0x8(%rdx),%rcx`),
  i.e. one load of the TLS slot followed by TLAB top/end, the same direct-TLAB consumer
  layout as before. The runtime publishes `Mutator::tlab()` (`&allocBuffer`) from
  `ThreadLocalData::SetMutator` (`runtime/src/Mutator/ThreadLocal.cpp:96`, resolved with
  clice against the 853e Release compilation database).
- The 853e native producer (`build_runtime.mjs` → `build.py build/install`, CPP suite FRESH)
  and the 6a26 installed-language gate on this package ran the finalizer fixture
  (`finalized=64 expected=64`) and the minor/major phase-entry guards with rc 0
  (reports `REPORT-opus_reference_qualification_1121.md`, `REPORT-review_reference_native_853e_1121.md`).
- From the unpacked archive the public cjc compiled and ran three ordinary consumers against
  the packaged target runtime: synchronized C writes to live acquired arrays (120/120),
  held/unheld array address observation over 36 and 48 rounds, and a two-thread minor-GC
  mutator (63 and 148 minor collections); all rc 0 (`REPORT-ffi_language_minor_1122.md`).

This qualifies the current AOT ABI pairing for the managed gate only; it is not a release
or performance claim. `language.runtime` binds the fixed SDK runtime; a runtime commit
containing this record is admitted as a separately measured target through the unchanged
colour checker. See `LANGUAGE_TOOLCHAIN.md`.
