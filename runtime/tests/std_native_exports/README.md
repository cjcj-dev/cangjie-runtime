# Public std native export consumer

Build and run `main.cj` through the ordinary compiler driver against the newly built and independently installed std CJO and its matching runtime libraries. Do not resolve CJO from an older SDK while loading candidate SOs. The consumer uses public env, process and console APIs; it does not declare or call private `CJ_OS_*`, `CJ_CONSOLE_*` or `CJ_BUFFER_*` functions.

The candidate shared env/process/console libraries must keep these native implementations local while still satisfying every ordinary CJO reference. Preserve the generated link commands, each shared library's dynamic symbol table and native archive membership. Check static linking separately: the archive symbols must remain available. On ELF, both envFFI and processFFI archive symbols are excluded. On Darwin, `-load_hidden` takes actual archive paths and extracts referenced members; it is not force loading. Verify the resulting dependency closure on Mach-O. MinGW retains its current export behavior because its linker has no `--exclude-libs` option.

This source has not been compiled or run merely because it is present. Full std/CJO production, installed consumer execution, old-binary compatibility and static archive controls remain required with a stable paired toolchain. Platform results must be recorded separately.
