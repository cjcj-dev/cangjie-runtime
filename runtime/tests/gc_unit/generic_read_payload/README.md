# Generic field payload read regression

Use a normally installed SDK with its paired std/runtime, on a supported native target. The library deliberately has no `@Frozen` body export. Its consumer must execute the library generic field-read path for both an Int64 payload and a String-bearing value payload.

The generic container itself always has a separate String reference. The primitive arm therefore distinguishes the copied value's TypeInfo from the container's TypeInfo.

Run under the task resource wrapper (core limit zero):

```text
zx run.mjs build <SDK> <this-directory> <output-directory> <CPU-list>
zx run.mjs run <SDK> <output-directory> <installed-runtime-library-directory> <label>
```

Both target processes must return 0 and print their assertion-reached markers. The driver preserves compiler and process rc, stdout/stderr, loaded-library evidence and executed ELF/SO identities. It performs no timing or workload sampling.

For product-wiring falsification in an isolated authorized source/build: replace only the no-reference branch's payload copy in CJ_MCC_ReadGeneric with zero-fill; rebuild and install that runtime normally. The primitive assertion must return 1 with its marker, while the reference-bearing assertion stays 0. Restore the actual copy, rebuild/install, and both return 0. Do not change assertions or the library/consumer sources between these arms.
