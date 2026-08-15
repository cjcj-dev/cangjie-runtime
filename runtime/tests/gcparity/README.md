# GC parity release benchmark

This directory contains the fixed workloads used by `REPORT-gcparity.md`.
They intentionally exercise different ends of the survival spectrum:

- `allocation_dense.cj` allocates short-lived objects while retaining only a
  small old cohort.
- `survival_dense.cj` keeps a large rotating root set alive long enough to
  cross collection boundaries.

Both programs are deterministic and print one terminal `*_OK` record.  A run
is correct only when it exits zero and the SHA-256 of stdout is identical
between the subject and official arms.  Runtime logs must therefore be sent to
separate files rather than mixed into stdout.

The release gate is evaluated independently for every workload, heap size, and
`MRT_GCV2_FULL_YOUNG_SCAN` value.  It does not average a losing workload away:

1. each arm has at least 20 interleaved attempts and every attempt is correct;
2. subject/control median wall-time is at most `1.00`;
3. subject/control p99 pause is at most `1.00`;
4. subject/control median maximum RSS is at most `1.00`; and
5. either wall-time or p99 pause is at most `0.95`.

A pause sample is an emitted `stw time` or `light sync time` interval.  This is
available in both the official non-generational collector and the subject;
the subject-only `[GCV2Minor] pause` field is not mixed into the cross-version
metric.  p99 uses the nearest-rank definition.  GC cycles are counted from
`Begin GC log. GCReason:` records and are reported both in full and on an
equal-cycle paired subset when one exists.
