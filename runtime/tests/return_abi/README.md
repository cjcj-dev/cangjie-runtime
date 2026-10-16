# Linux x64 generated return ABI input

Compile native.c as a shared library and main.cj with a qualified Cangjie SDK,
linking that library. Use the existing compiler and runtime build entries;
retain the compiler command, generated binding objects, SDK lock, ELF and SO
hashes. The CFunc lambdas must be emitted by the compiler, and their actual
N2C stub route and return classifications must be checked in the generated
objects before interpreting runtime results.

RunUInt compares integer bits, including the actual product stub start,
interior, final byte and both outside boundaries. These integers are never
dereferenced. RunPair and RunFloat exercise actual signature classification;
do not infer register classes solely from C declarations.

The EH targets throw the same ReturnError and consume it through a typed
managed catch. Mutating the caught object and reading the original reference
checks identity. CrossPending returns from the generated callback to native,
then lets the real C2N leave consume pending state. No EH result value is
asserted. A missing input, compilation/loading error or earlier failure is
not a target red arm. Successful assertions print RETURN_ABI_TARGET.

The normal cut restores the BASE N2C address-based clearing and must fail
only the three in-range integer targets, with outside/other-signature controls
green. The EH cut must be at actual restoration/consumption, reach the EH
target assertion and restore green; an earlier runtime assertion or timeout
is not acceptance evidence. This input alone does not prove watermark/root
safety or other platform ABI obligations.
