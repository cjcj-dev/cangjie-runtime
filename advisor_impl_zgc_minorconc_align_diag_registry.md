# impl_zgc_minorconc_align diag registry ownership

Frozen gate on kkk2 stopped before build at diagreg because this task correctly deletes three runtime env names, while shared `/root/cj_build/tools/diag_registry.sh` still lists them:

- `MRT_GCV2_YOUNG_CONC_FOLLOW`
- `MRT_GCV2_YOUNG_CONC_MARK`
- `MRT_GCV2_YOUNG_MARK_END_FORCE_REENTER`

Evidence: `kkk2:/root/impl_zgc_minorconc_align_evidence/gate_all.log`, `SEG_RC[diagreg]=1`; no build/unit/nwdet ran.

Should this runtime implement lane also patch the tools registry and deliver a second tools patch, or will the main controller update the shared registry and re-dispatch the gate? The runtime candidate must not restore any deleted env just to satisfy the stale registry.
