# Native return-reference pair

`return-poll-native.yml` runs `native.py` with a runtime checkout on macOS arm64/x86_64 and Windows. This is the LLVM #74 experiment: the native bridge installs the managed TLS register, real llc output issues the return poll, and the product names the returned reference in a Handle before a handshake rewrites it. `PAIR_RETURN_TARGET` checks the returned reference and saferegion state.

`objects.json` contains actual llc-produced object bytes, input and tool SHA256, and source identities. Apple inputs were emitted with LLVM #81 head 05afd9ce1af44eb2a47fbb067cd43fb274e5a864. Its producer cut removes CJMetadata.cpp:525-526 map address emission. Windows uses the unchanged LLVM #74 objects (return-poll producer cut). These are different platform producers, explicitly identified in the manifest. The runner validates the IR and object hashes before linking.

The consumer cut removes the existing CollectReturnRegisterRoots call in HandleReturnSafepoint. Candidate, consumer cut and restored runs use the same executable; restored uses the preserved candidate library bytes. Each arm must execute the target assertion; build/load failures do not count as a red arm.

The Apple map contract is a 16-byte relocated {u64 startPC, u64 descriptor} record in __CJ_METADATA,__cjfuncmap, with no header. Runtime sorts on image registration and retires the index with its owning image. The returned frame is never used to locate the descriptor.
