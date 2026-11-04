# Native safety regressions

`rwlock.cpp` consumes the real RwLock header. Compile with the product include directories, C++17 and pthread, then run. For deterministic memory-order regression, also compile with Clang `-O1 -S -emit-llvm`: the `cmpxchg weak` in each of `NativeSafetyLockWrite` and `NativeSafetyTryLockWrite` must have success `acq_rel`, failure `monotonic`. Reverting either product CAS to `release` must fail that check. The contention run alone on x86 cannot prove weak-memory correctness.
