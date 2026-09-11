# Forwarding return-domain contract

Run `run_forward_domain_arms.py` on the build host with an existing testable
runtime build and its `cj_gc_unit`. It selects each test by its exact name,
retains both shared libraries and one fixed test executable, and records rc,
startup, both done states, named target assertions, hashes and build times.
`CJ_GC_UNIT_FORWARD_DOMAIN=1` enables the six `ForwardReturnDomain` tests.
Ordinary unit invocations print `DOMAIN NOT_RUN`; that is not contract coverage.
The frozen main guard rejects valid identity, so `reject` is intentionally red
for identity and retired identity. `accept` applies the existing #175 control
(`resolved != nullptr`) transiently to the product SO. This test change does not
ship #62's guard modification.

The setup extends #175's real young-GC dispatch, kept publisher and copying
worker. The TLS test-driver thread is not a registered managed mutator. For
copy, it enters during the prepared/flipped POST_TRACE pause, including while
STW is held by this test runtime; the product's phase check refuses mutator
copying and naturally enters WaitRoutedTipReady. The GC then resumes its real
copy operation. This verifies that product path, not managed-thread scheduling
or safepoint participation. The hooks only pause execution and are compiled
under MRT_TESTABLE_INTERNALS.

Each case observes the real producer receipt, releases a driver already parked
inside WaitRoutedTipReady, and checks the value returned by the product remap
entry. Identity, real non-identity copy, and retired identity each run before
and after the product writes done. Negative inputs are constructed only after
real publication: remove the selected receipt, change its region life while
keeping the diagnostic route current, or seal publication after removal. They
must terminate at the actual product CHECK, with the specific reason and
consumer text; an arbitrary signal or a different earlier CHECK does not pass.
The parent records both done scenarios before asserting the combined verdict.

WrongLifecycle rejection is limited to CJRT_LIFECLOCK_ENFORCE=1. The same case
also runs with LIFECLOCK_AUDIT=1 and enforcement off, requiring ArmedHit and the
returned identity. This is the existing staged life-clock contract, not a new
default-enforcement claim.

## WaitRoutedTipReady return-point inventory

Coordinates below refer to frozen main
68e1c6a54b2a5967a783580ff9d358b281fb39e6, in
runtime/src/Heap/Collector/Relocate.cpp. Hooks added by this change shift lines;
the quoted return kind is the stable identifier.

| Exit | Domain and authority | Evidence scope |
|---|---|---|
| 2134 `observeReturn` | Returns exactly its argument; diagnostic wrapper only | Read; callers below |
| 2145 CHECK in `permanentHole`; 2172 unreachable null return | Missing / wrong life / unavailable; terminates, never invents an address | MissingEntry, WrongLifecycle(enforce), Unavailable |
| 2182 `lookupTo` non-null | Only `ArmedHit` with nonzero `lastLookup.to`; may be identity, copy or retired hit | All three valid-domain cases |
| 2184 `lookupTo` null | Other answers, including miss and unavailable | Three negative cases |
| 2191 `initial-lookup` | Valid table result, heap address and valid object | Identity, NonIdentityCopy, RetiredHit; each done side |
| 2201 `permanentHole(reason)` | Publication closed / unavailable; reason distinguishes lifecycle from never installed | WrongLifecycle, Unavailable; each done side |
| 2228 `unpublished-use-to` | The same `lookupTo` non-null answer | Read only; not separately executed by these cases |
| 2250 `published-without-receipt` | Published miss, no geometry fallback | MissingEntry after done |
| 2273 `ineligible-lookup` | Re-query through the same validated lookup | Read only; not separately executed |
| 2276 `ineligible-closed` | Returns nullptr, so outer guard must reject | Read only; not separately executed |
| 2281 `published-without-receipt` | Ineligible wait has no receipt | Read only; not separately executed |
| 2294 `regionIsPublished` | Boolean done predicate for request queue, not an object return | Read only |
| 2305 `retain-refused-lookup` | Re-query through the same validated lookup | Read only; not separately executed |
| 2307 `retain-refused-without-receipt` | Retain refused without any answer | MissingEntry before done |
| 2331 `fwdDone-timeout` | Queue wait timed out; fail closed | Read only; not separately executed |
| 2340 `request-receipt` | Nonzero queue receipt, heap address and valid object | Read only; publication goes through requests.Publish, not an original-address fallback |
| 2353 `terminal-lookup` | Re-query through the same validated lookup | Read only; not separately executed |
| 2359 `request-complete-without-receipt` | Completed wait with no queue or table receipt | Read only; not separately executed |
| 2361 `forwarding-table-miss` | Remaining no-answer path fails closed | Read only; not separately executed |

This inventory establishes the absence of an unconditional original-address
fallback by reading every object-return site. The six tests cover answer domains
at the initial lookup and specified failure exits; they do not claim dynamic
coverage of every queue/interleaving branch in the inventory.
