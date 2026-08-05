#!/usr/bin/env python3
import os, struct, re, glob, subprocess
from collections import Counter

def get_bias(maps_text):
    all_lo = None
    for line in maps_text.splitlines():
        if "natural_wave" not in line:
            continue
        m = re.match(r"([0-9a-f]+)-", line)
        if m:
            lo = int(m.group(1), 16)
            if all_lo is None or lo < all_lo:
                all_lo = lo
    return all_lo or 0x555555554000

def load_syms(path):
    out = subprocess.check_output(["nm", "-C", path], text=True, errors="replace")
    syms = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        try:
            a = int(parts[0], 16)
        except Exception:
            continue
        syms.append((a, parts[1], " ".join(parts[2:])))
    syms.sort()
    return syms

SYMS = load_syms("/root/b2trap-run/bin/natural_wave")
BIAS = [0x555555554000]

def tip_name(v):
    bias = BIAS[0]
    if v is None or not (bias <= v < bias + 0x400000):
        return None
    fa = v - bias
    best = None
    for a, t, n in SYMS:
        if a <= fa:
            best = n
        else:
            break
    return best

def find_file(run, addr):
    for p in glob.glob(run + "/mem_*.bin"):
        bn = os.path.basename(p)
        m = re.match(r"mem_([a-z]+)_([0-9a-f]+)-([0-9a-f]+)\.bin", bn)
        if not m:
            continue
        lo, hi = int(m.group(2), 16), int(m.group(3), 16)
        if lo <= addr < hi:
            return lo, hi, p
    return None

def ru64(path, lo, addr):
    with open(path, "rb") as f:
        f.seek(addr - lo)
        b = f.read(8)
        return struct.unpack("<Q", b)[0] if len(b) == 8 else None

def words_at(run, addr, offs=range(-16, 32, 8)):
    mf = find_file(run, addr)
    if not mf:
        return None, {}
    lo, hi, path = mf
    w = {}
    for o in offs:
        a = addr + o
        if lo <= a and a + 8 <= hi:
            w[o] = ru64(path, lo, a)
    return (lo, hi, path), w

def classify_ptr(v, wm16=None):
    tn = tip_name(v)
    tnm = tip_name(wm16) if wm16 is not None else None
    if tnm and str(tnm).endswith(".ti") and tn and not str(tn).endswith(".ti"):
        return "INTERIOR16", tnm, tn
    if tn and str(tn).endswith(".ti"):
        return "BASE", tn, None
    if tnm and str(tnm).endswith(".ti"):
        return "INTERIOR_ISH", tnm, tn
    return "OTHER", tn, tnm

family = []
with open("/root/b2trap-run/evidence/dump60_matrix.tsv") as f:
    f.readline()
    for line in f:
        p = line.rstrip("\n").split("\t")
        if len(p) < 5:
            continue
        if p[4] == "1" or p[3] in ("ABRT_OTHER", "SIZEGUARD", "ISMARKED", "SEGV", "INVALID_OBJ"):
            family.append(p)

results = []
print("FAMILY_N", len(family))
for p in family:
    rnd = int(p[1])
    cls = p[3]
    run = f"/root/b2trap-run/runs/d2_r{rnd}"
    print("=" * 70)
    print(f"d2_r{rnd} class={cls}")
    if not os.path.isdir(run):
        print(" MISSING")
        continue
    err = open(run + "/stderr.txt").read()
    sup = open(run + "/supervisor.log").read() if os.path.exists(run + "/supervisor.log") else ""
    # fix bias from this run maps
    maps_p = None
    for cand in sorted(glob.glob(run + "/maps_*.txt")):
        if "abrt" in cand or "segv" in cand:
            maps_p = cand
            break
    if maps_p:
        BIAS[0] = get_bias(open(maps_p).read())

    fatals = [l for l in err.splitlines() if re.search(r"( F |SIGSEGV|SIGABRT|Invalid object|Check failed|sizeguard)", l)]
    print(" fatals:", fatals[-3:])
    er = re.search(r"EXE_RANGE ([0-9a-f]+)-([0-9a-f]+)", sup)
    print(" EXE_RANGE", er.group(0) if er else None, "B2GEO", "B2GEO" in sup, "INTERIOR16_REG", "INTERIOR16_REG" in sup)

    sg = re.search(
        r"obj=(0x[0-9a-f]+) objSize=(\d+) region=(0x[0-9a-f]+) regionStart=(0x[0-9a-f]+) regionEnd=(0x[0-9a-f]+)",
        err,
    )
    inv = re.search(
        r"Invalid object (0x[0-9a-f]+) is referenced by strong object (0x[0-9a-f]+).*offset (\d+)",
        err,
    )

    if sg:
        obj = int(sg.group(1), 16)
        mf, w = words_at(run, obj)
        print(f" SG obj={obj:#x} size={sg.group(2)}")
        if not mf:
            print("  NO_MEM for obj")
            results.append((rnd, cls, "SG_NO_MEM"))
        else:
            for o in sorted(w):
                print(f"  [{o:+d}] {w[o]:#x} {tip_name(w[o])}")
            kind, a, b = classify_ptr(w.get(0), w.get(-16))
            print(f"  geometry={kind} true_ti={a} *V={b}")
            # also true base layout
            mf2, w2 = words_at(run, obj - 16)
            if mf2:
                print("  truebase layout:")
                for o in sorted(w2):
                    print(f"    [{o:+d}] {w2[o]:#x} {tip_name(w2[o])}")
            # find Node holders
            hits = []
            for path in glob.glob(run + "/mem_*.bin"):
                bn = os.path.basename(path)
                m = re.match(r"mem_[a-z]+_([0-9a-f]+)-([0-9a-f]+)\.bin", bn)
                if not m:
                    continue
                lo, hi = int(m.group(1), 16), int(m.group(2), 16)
                if lo < 0x7fff00000000:
                    continue
                sz = hi - lo
                if sz > 120 * 1024 * 1024:
                    continue
                with open(path, "rb") as f:
                    data = f.read()
                for i in range(0, len(data) - 8, 8):
                    v = struct.unpack_from("<Q", data, i)[0]
                    if v != obj:
                        continue
                    if i >= 16:
                        tip = struct.unpack_from("<Q", data, i - 16)[0]
                        tn = tip_name(tip)
                        if tn and "Node" in tn:
                            hits.append((lo + i - 16, tn, lo + i))
                if len(hits) >= 8:
                    break
            print(f"  Node holders next==obj: {[(hex(h), t) for h, t, s in hits[:6]]}")
            if kind in ("INTERIOR16", "INTERIOR_ISH"):
                results.append((rnd, cls, "SG_INTERIOR_TRUE_OBJECT_INTACT", a, b, len(hits)))
            else:
                results.append((rnd, cls, f"SG_{kind}", a, b, len(hits)))

    if inv:
        obj = int(inv.group(1), 16)
        holder = int(inv.group(2), 16)
        off = int(inv.group(3))
        print(f" INV obj={obj:#x} holder={holder:#x} off={off}")
        mf, w = words_at(run, holder)
        if mf:
            print("  holder:")
            for o in sorted(w):
                print(f"    [{o:+d}] {w[o]:#x} {tip_name(w[o])}")
            slot = w.get(off)
            print(f"  slot+{off} val={slot:#x}" if slot else "  slot missing")
            if slot is not None:
                mf2, w2 = words_at(run, slot)
                if mf2:
                    print("  slot target:")
                    for o in sorted(w2):
                        print(f"    [{o:+d}] {w2[o]:#x} {tip_name(w2[o])}")
                    kind, a, b = classify_ptr(w2.get(0), w2.get(-16))
                    print(f"  slot_geometry={kind} true_ti={a} *V={b}")
                    # true base intact?
                    mf3, w3 = words_at(run, slot - 16)
                    true_alive = False
                    if mf3 and tip_name(w3.get(0)) and str(tip_name(w3.get(0))).endswith(".ti"):
                        true_alive = True
                    if kind in ("INTERIOR16", "INTERIOR_ISH") and true_alive:
                        results.append((rnd, cls, "SLOT_HOLDS_INTERIOR_TRUE_ALIVE", a, b))
                    elif kind in ("INTERIOR16", "INTERIOR_ISH") and not true_alive:
                        results.append((rnd, cls, "SLOT_HOLDS_INTERIOR_TRUE_DEAD", a, b))
                    elif kind == "BASE":
                        results.append((rnd, cls, "SLOT_POINTS_TO_BASE", a, b))
                    else:
                        # memory reused? no ti at slot or slot-16
                        if not tip_name(w2.get(0)) and not tip_name(w2.get(-16)):
                            results.append((rnd, cls, "SLOT_TARGET_NO_TI_POSSIBLE_REUSE", None, None))
                        else:
                            results.append((rnd, cls, f"SLOT_{kind}", a, b))
                else:
                    print("  slot target NO_MEM")
                    results.append((rnd, cls, "SLOT_TARGET_NO_MEM"))
        else:
            print("  holder NO_MEM")
            results.append((rnd, cls, "HOLDER_NO_MEM"))

        # also classify inv obj itself
        mf, w = words_at(run, obj)
        if mf:
            kind, a, b = classify_ptr(w.get(0), w.get(-16))
            print(f"  inv_obj geometry={kind} true_ti={a} *V={b}")

    if cls in ("ISMARKED", "SEGV", "ABRT_OTHER", "TOREGION2") and not sg and not inv:
        m = re.search(
            r"REGS fatal\s+tid=\d+\s+rip=([0-9a-f]+)\s+rax=([0-9a-f]+)\s+rbx=([0-9a-f]+)\s+rcx=([0-9a-f]+)\s+rdx=([0-9a-f]+)\s+rsi=([0-9a-f]+)\s+rdi=([0-9a-f]+)\s+rbp=([0-9a-f]+)\s+rsp=([0-9a-f]+)\s+r8=([0-9a-f]+)\s+r9=([0-9a-f]+)\s+r10=([0-9a-f]+)\s+r11=([0-9a-f]+)\s+r12=([0-9a-f]+)\s+r13=([0-9a-f]+)\s+r14=([0-9a-f]+)\s+r15=([0-9a-f]+)",
            sup,
        )
        found = 0
        if m:
            vals = [int(x, 16) for x in m.groups()]
            names = ["rip", "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"]
            for nm, v in zip(names, vals):
                if (v >> 40) != 0x7F:
                    continue
                mf, w = words_at(run, v)
                if not mf:
                    continue
                kind, a, b = classify_ptr(w.get(0), w.get(-16))
                if kind in ("INTERIOR16", "INTERIOR_ISH"):
                    print(f"  REG {nm}={v:#x} {kind} true_ti={a} *V={b}")
                    results.append((rnd, cls, f"REG_{nm}_{kind}", a, b))
                    found += 1
                elif kind == "BASE" and a and "Node" in str(a):
                    print(f"  REG {nm}={v:#x} BASE Node")
        # also search stderr for more context
        if "toRegion2" in err:
            results.append((rnd, cls, "TOREGION2_RAW"))
        elif "IsMarkedObject" in err:
            results.append((rnd, cls, "ISMARKED_RAW" if not found else "ISMARKED_WITH_INTERIOR_REG"))
        elif "SIGSEGV" in err or cls == "SEGV":
            results.append((rnd, cls, "SEGV_RAW" if not found else "SEGV_WITH_INTERIOR_REG"))
        else:
            # dump check failed line
            for l in fatals:
                if "Check failed" in l:
                    print("  ", l[-120:])
                    results.append((rnd, cls, "CHECK_OTHER", l[-80:]))
                    break
            else:
                if not found:
                    results.append((rnd, cls, "UNCLASSIFIED_FATAL"))

print("=" * 70)
print("RESULTS")
c = Counter(r[2] for r in results)
for k, v in c.most_common():
    print(f"  {v:3d}  {k}")
for r in results:
    print(" ", r)

# Verdict logic
interior_alive = sum(1 for r in results if "INTERIOR" in r[2] and "DEAD" not in r[2] and "TRUE_OBJECT_INTACT" in r[2] or "TRUE_ALIVE" in r[2] or "INTERIOR16" in r[2] or "INTERIOR_ISH" in r[2] or "SG_INTERIOR" in r[2])
reuse = sum(1 for r in results if "REUSE" in r[2] or "DEAD" in r[2])
print("interior_alive_score", interior_alive, "reuse_score", reuse)
