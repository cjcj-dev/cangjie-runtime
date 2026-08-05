# b2trap — zero-hot-path forensics (ptrace supervisor)

- `b2trap_supervisor.c` — ptrace parent; optional DR0-3 write watchpoints; fatal-signal dump
- `posctrl.c` — device-level positive control (3 writes / abort path)

## Build

```bash
cc -O2 -o b2trap_supervisor b2trap_supervisor.c
cc -O0 -no-pie -o posctrl posctrl.c -lpthread
```

## Pure dump arm (main)

```bash
./b2trap_supervisor -o /root/b2trap-run/runs/d_rN -t 120 -F 2 -- /path/to/natural_wave
```

No `-w` ⇒ zero watchpoints, hot path unmodified. First 2 fatals full RW dump; later bounded.

## Confirm arm (after offline slot known)

```bash
./b2trap_supervisor -o out -w 0xSLOT -- /path/to/natural_wave
```

## Mid-run arm (Node.next @ +16)

```bash
# plant INT3 on CJ_MCC_NewObjectFast ret; on Node alloc, DR-watch obj+16
./b2trap_supervisor -o out -A node -S 1 -M 0 -t 120 -F 2 -- /path/to/natural_wave
# -S N  arm every Nth Node (default 1)
# -M N  stop after N armed slots (0=unlimited; only 4 DRs, RR replace)
```
