# Default-disabled backup timers

ZGC `z_globals.hpp:62-66` defaults both collection timers to disabled;
`zDirector.cpp:84-100,385-400` checks nonpositive intervals before expiry.
The public unsigned `backupGCInterval` uses zero for the disabled default.

On kkk2, compile `runtime_timer.cpp` with `-g -std=c++17 -I runtime/src`, linking
only the product `libcangjie-runtime.so`, as in `worker_config/README.md`.
Reuse this same ELF across candidate, producer-cut, consumer-cut and restored SOs.
Run `bash run.sh` with `TIMER_ELF`, `TIMER_SOURCE` (matching zDirector.cpp),
`GCV2_RUNTIME_LIB_DIR` (runtime and boundscheck SOs), `TIMER_OUT`, and the leased
`TIMER_CPUSET`. Eight cases run concurrently: env/API × default/1s × major/minor.

The debugger stops the real director before sampling, holds other threads,
waits 241 seconds, then lets the real sampling, decision and port send complete.
It reads the resulting product port message. The minor cases establish a busy
major through product methods, like the existing director busy matrix. No clock,
statistics, decision, or message output is overwritten. Product identity, input
statistics, elapsed time and the exact dispatch assertion are printed.

Positive control: explicit 1s must dispatch BACKUP. Default must not. Restoring
both old 240s producers must fail exactly the four default cases. Clearing the
sampled interval at the real director entry must fail exactly the four explicit
cases. `TIMER_WAIT_SECONDS` may shorten a harness development run; only runs
exceeding 240 seconds qualify the producer-cut assertion.

The retired TaskQueue TIMEOUT path is outside this test: controller ruling
`sym_cangjie_runtime_910_implement_r5785397551-20260922T224228Z.md` assigns its
removal to the #898 driver package and approves observing current director ports.
