# Installed language input contract

The language compiler is `GC_UNIT_LANGUAGE_SDK/bin/cjc`. Its entry recipe selects
`GC_UNIT_CJC_RUNTIME_LIB_DIR`; the target pair is `GCV2_RUNTIME_LIB_DIR` and must
match that SDK. `GC_UNIT_RUNTIME_TUPLE` selects a native Linux ELF ABI.
`GC_UNIT_COLOUR_HOST_RUNTIME` is the plain reference used by the existing
std/runtime colour-export check, not an approved identity. Compiler-inputs.json
records the actual compiler source and bytes; no approved tuple is embedded.

The gate records compiler, launcher, LLVM, std archives, modules, compiler-host
libraries, target pair and checker bytes in TOOLCHAIN_IDENTITY. Installation
cache verification compares those actual inputs; moving identical bytes does
not change identity. The original native configuration/pair remains mandatory.
Only real language tests and actual loader observations can produce LANGUAGE_DONE.
