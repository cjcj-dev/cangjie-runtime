# Reflected CR01 constructor receiver

Source pairing, not production qualification:

- C `03f2c2753f189d00a8d75363ce7a83bd687789b5`:
  `packages/chir/src/Utils.cj:948` excludes STATIC/FOREIGN/non-class
  constructors from `ConstructorReturnsReceiver`;
  `FaithfulAST2CHIR.cj:4700` gives those internal class constructors the
  receiver reference return type, and other constructors Unit.
  `packages/codegen/src/CJNativeGenMetadata.cj:257` reads that actual
  `method.GetType()` return, while `GetTiName` strips Ref at199.
  `GenerateClassLikeMethodMetadata` at470 routes real class methods there.
- N `de7f37458b456c4a7ef1ebe2726d81fcfd9904c2`:
  `llvm/lib/Transforms/Scalar/CJFillMetadata.cpp:1285` parses that
  MRT_RETURN_TYPE; `fillMethodInfo` at719 writes its TI into MethodInfo.
  No new modifier bit or table layout is needed by this runtime change.
- R's public managed consumer is `ClassTypeInfo.construct` /
  `ConstructorInfo.apply` in `stdlib/libs/std/reflect`, then
  `applyCJStaticMethod`, the platform `CJ_MCC_ApplyCJStaticMethod` stub,
  `CompilerCalls.cpp:MCC_ApplyCJStaticMethod`, `MethodInfo::ApplyCJMethod`,
  and `ApplyCangjieMethodStub`. Generic class return metadata is resolved with GetReturnType before the
  call and must match the concrete declaring TI. Unit FOREIGN signatures
  retain the old return convention. Unresolved/mismatched class signatures
  are errors, not a reason to select the Unit path.

`ApplyCJMethodImpl` registers a normal class-constructor result in the
existing receiver Handle before Unit normalization or later metadata work.
Return classification is cached before the call: GetReturnType can enter
GetOrCreateTypeInfo and must not run while the result is still an unrooted raw
reference. The C author confirmed this at T170249Z; old managed Unit metadata
is indistinguishable from new FOREIGN Unit, so all target std/CJO/DSO inputs
must be rebuilt with the new qualified pair.
The final initializer path consumes this updated Handle. Struct sret/value
branches and static calls do not select this rule.

`ThrowPendingException` is not C++ noreturn: `ExceptionHandling` constructs
EH frame information and changes a managed return edge. The pending branch
therefore explicitly stops normal result processing. The outer class
initializer returns no instance on that edge, while its native buffers and
HandleMark undergo normal C++ scope cleanup. This is exception propagation,
not a null-success alternative for a failed normal constructor.

The fixed ZGC reference is
`5b2d6991a1279d375f9a3c00c7bcd0bbcc7081d6`:
`runtime/handles.inline.hpp:43` allocates a thread-private Handle;
`runtime/reflection.cpp:1110` passes rooted arguments, separates exception
from normal result handling, and its constructor at1163 roots the receiver.
The infrastructure difference is explicit: HotSpot Java constructors are
void, whereas CR01 Cangjie constructors return the updated receiver after
Object.init/super/this. The new Cangjie return is kept in the same existing
thread-private root representation, not in a second persistent storage.

## 产品接线证明

All new execution below is **NOT_RUN** until the paired source defects have
been independently checked and the required normal inputs are qualified.
Existing f5ea/65ab/9ee execution is not relabelled as this candidate.

The existing `gc_unit` CMake target already includes `test_zPage.cpp`.
New tests use the real runtime lifecycle, task, exported MCC entry,
MethodInfo argument builder, architecture call stub, collector and Handle
storage. Only the callee and its MethodInfo are native test inputs:

- `ReflectedConstructor.ClassReturnForwardsReceiver`: the callee returns a
  distinct rooted object after collection. The assertion demands that exact
  result, its marker202 and balanced native roots. The original receiver has
  marker101, so returning the old receiver cannot silently pass.
- `ReflectedConstructor.UnitAbiIgnoresRegisterResult`: identical call shape
  with Unit return metadata must retain the original receiver/marker101.
  This checks Unit-ABI consumption; it does not claim a real FOREIGN source
  program has run.

The planned product cut removes the actual receiver Handle update in
`ApplyCJMethodImpl`; it leaves the stub call, returned Value, metadata,
assertions and Unit control intact. Require target assertion red after
`REFLECTION_CTOR_TARGET executed=1`, Unit control green, then restore and
rebuild/install before rerunning both. Preserve cut.diff/commands/own rc.
This is a new result-consumption cut, not a claim about a pre-existing
baseline entry_cut. The baseline runtime's old receiver return can be
checked with the same new test inputs when its normal build is admitted.

## 承重面清单

- Linux native default/testable: normal `runtime/build.py build`/install
  with the original bounds dependency and existing CMake gc_unit target.
  Invoke installed-product-linked unit executable with
  `--gtest_filter=ReflectedConstructor.ClassReturnForwardsReceiver` or
  `--gtest_filter=ReflectedConstructor.UnitAbiIgnoresRegisterResult`.
  Capture actual runtime/bounds loaded paths and ELF/SO identity.
- Affected existing controls:
  `NativeArgumentHandle.CallbackRetainsReceiverAndArguments` (Unit receiver
  and argument roots), `NativeReturnHandle.HeaderlessResultAfterCollection`
  (struct sret roots). These are affected by the shared invocation helper;
  other completed GC/STS/attach and Windows CLI evidence need not be rerun.
- Real language consumer `reflection_constructor_product.cj`: compile via
  the qualified installed compiler at O0 and O2 with its matching N and
  newly produced std, then run from that install. Seven named assertions
  cover super/this fields, generic class roots, fixed/generic struct ABI,
  target exception without a normal result, and a successful call after
  the exception. Preserve complete compile child/run child rc, output and
  identity. Repeat the consumer from the unpack of this same install.
  No old std, unqualified8ba ELF or mismatched N may stand in for that pair.
- Actual FOREIGN language consumer, independent CJO import/inline metadata,
  Windows DLL, AArch64/PAuth and other platforms remain explicit uncovered
  boundaries until their normal paired inputs and applicable execution.
  No x86 macro simulation or native fixture discharges them.

Native builds use the original kkk2 box+wf owner domain, 4 CPUs/16GiB,
Swap0/core0 and cumulative16GiB entity. Reuse existing default/testable
caches, qualify actual disk space before creating new install/package
outputs, and do not modify frozen dae4/f5ea product inputs. New peak is
unknown until an admitted execution; previous peaks are not estimates of
this candidate. Source changes are not permission to bypass the current
paired-production STOP.

## 测试增删

Added two native ABI-consumer tests and one real managed source with seven
assertions. No existing test is deleted, renamed or weakened. The source
is prepared, not a passing managed lifecycle or exception result.

## FALSIFIED

The earlier investigation lacked the compiler metadata producer and treated
FOREIGN discrimination as unresolved. Reading the complete fixed producer
shows existing return-type metadata already carries the difference.
Likewise, treating ThrowPendingException as C++ noreturn would allow
invalid return bits into a new root; its real implementation returns after
redirecting the managed edge. Neither finding is a runtime measurement.
