#!/usr/bin/env python3
"""Offline analysis of b2trap dumps: answer
  Is Node.next written as interior, or is memory reused under a stable slot?
"""
import os, sys, struct, re, glob

ROOT = sys.argv[1] if len(sys.argv) > 1 else "/root/b2trap-run"
DUMPS = os.path.join(ROOT, "dumps")

def parse_sizeguard(stderr):
    # obj=0x.. objSize=.. region=0x.. regionStart=.. regionEnd=..
    m = re.search(
        r'obj=(0x[0-9a-f]+)\s+objSize=(\d+)\s+region=(0x[0-9a-f]+)\s+regionStart=(0x[0-9a-f]+)\s+regionEnd=(0x[0-9a-f]+)',
        stderr)
    if not m:
        return None
    return dict(obj=int(m.group(1),16), size=int(m.group(2)), region=int(m.group(3),16),
                rstart=int(m.group(4),16), rend=int(m.group(5),16))

def parse_invalid(stderr):
    # Invalid object 0xA is referenced by strong object 0xB: default:Node and offset 16
    m = re.search(r'Invalid object (0x[0-9a-f]+) is referenced by strong object (0x[0-9a-f]+).*offset (\d+)', stderr)
    if not m:
        return None
    return dict(obj=int(m.group(1),16), holder=int(m.group(2),16), offset=int(m.group(3)))

def load_maps(path):
    maps = []
    with open(path) as f:
        for line in f:
            m = re.match(r'([0-9a-f]+)-([0-9a-f]+)\s+(\S+)', line)
            if m:
                maps.append((int(m.group(1),16), int(m.group(2),16), m.group(3), line.strip()))
    return maps

def find_mem_file(run_dir, addr, tag_pref=("abrt","segv","fatal")):
    # mem_TAG_lo-hi.bin
    cands = []
    for p in glob.glob(os.path.join(run_dir, "mem_*.bin")):
        bn = os.path.basename(p)
        m = re.match(r'mem_([a-z]+)_([0-9a-f]+)-([0-9a-f]+)\.bin', bn)
        if not m: continue
        tag, lo, hi = m.group(1), int(m.group(2),16), int(m.group(3),16)
        if lo <= addr < hi:
            cands.append((tag, lo, hi, p))
    for pref in tag_pref:
        for c in cands:
            if c[0]==pref: return c
    return cands[0] if cands else None

def read_u64(path, lo, addr):
    off = addr - lo
    with open(path, 'rb') as f:
        f.seek(off)
        b = f.read(8)
        if len(b)<8: return None
        return struct.unpack('<Q', b)[0]

def is_likely_exe(v, exe_ranges):
    for lo,hi in exe_ranges:
        if lo <= v < hi: return True
    return False

def analyze_run(run_dir):
    stderr = open(os.path.join(run_dir,"stderr.txt")).read() if os.path.exists(os.path.join(run_dir,"stderr.txt")) else ""
    sup = open(os.path.join(run_dir,"supervisor.log")).read() if os.path.exists(os.path.join(run_dir,"supervisor.log")) else ""
    maps_path = None
    for cand in glob.glob(os.path.join(run_dir,"maps_*.txt")):
        if "abrt" in cand or "segv" in cand or "fatal" in cand:
            maps_path = cand; break
    if not maps_path:
        ms = glob.glob(os.path.join(run_dir,"maps_*.txt"))
        maps_path = ms[0] if ms else None
    maps = load_maps(maps_path) if maps_path else []
    exe_ranges = []
    for lo,hi,perm,line in maps:
        if "natural_wave" in line or "r-xp" in perm and "libcangjie" not in line and "/lib" not in line:
            if "natural_wave" in line or ("r-xp" in perm and lo < 0x600000000000):
                exe_ranges.append((lo,hi))
    # also from EXE_RANGE log
    m = re.search(r'EXE_RANGE ([0-9a-f]+)-([0-9a-f]+)', sup)
    if m:
        exe_ranges.append((int(m.group(1),16), int(m.group(2),16)))

    sg = parse_sizeguard(stderr)
    inv = parse_invalid(stderr)
    result = {"run": run_dir, "sg": sg, "inv": inv, "b2geo": "B2GEO_CANDIDATE" in sup,
              "interior16": "INTERIOR16_REG" in sup, "findings": []}

    targets = []
    if sg:
        targets.append(("sizeguard_obj", sg["obj"]))
        # classic B2: obj is interior = true_base+0x10; true base tip should be TypeInfo in exe
        targets.append(("sizeguard_obj_minus16", sg["obj"] - 16))
        targets.append(("sizeguard_obj_plus16", sg["obj"] + 16))
    if inv:
        targets.append(("invalid_obj", inv["obj"]))
        targets.append(("holder", inv["holder"]))
        targets.append(("holder_slot", inv["holder"] + inv["offset"]))
        targets.append(("invalid_obj_minus16", inv["obj"] - 16))

    for name, addr in targets:
        mf = find_mem_file(run_dir, addr)
        if not mf:
            result["findings"].append(f"{name}@{addr:#x}: NO_DUMP_COVERING")
            continue
        tag, lo, hi, path = mf
        # read word at addr and neighbors
        words = {}
        for off in (-32,-24,-16,-8,0,8,16,24,32):
            a = addr + off
            if a < lo or a+8 > hi: continue
            words[off] = read_u64(path, lo, a)
        tip0 = words.get(0)
        tip_m16 = words.get(-16)
        tip_m8 = words.get(-8)
        tip_p16 = words.get(16)
        cls = "unknown"
        if tip0 is not None:
            if is_likely_exe(tip0, exe_ranges):
                cls = "base_obj_TypeInfo_at_V"
            elif tip_m16 is not None and is_likely_exe(tip_m16, exe_ranges):
                cls = "INTERIOR_PLUS16_truebase_TypeInfo"
            elif tip_m8 is not None and is_likely_exe(tip_m8, exe_ranges):
                cls = "INTERIOR_PLUS8"
            elif tip0 == 0:
                cls = "null_header"
            else:
                cls = f"header={tip0:#x}"
        result["findings"].append(
            f"{name}@{addr:#x} dump={tag}[{lo:#x}-{hi:#x}] cls={cls} words=" +
            " ".join(f"{o:+d}:{words[o]:#x}" for o in sorted(words) if words[o] is not None)
        )
        # If this is holder_slot: the value IN the slot is the "next" pointer
        if name == "holder_slot" and tip0 is not None:
            slot_val = tip0
            # classify slot value geometry
            mf2 = find_mem_file(run_dir, slot_val)
            if mf2:
                t2,l2,h2,p2 = mf2
                hdr = read_u64(p2, l2, slot_val)
                hdr_m16 = read_u64(p2, l2, slot_val-16) if slot_val-16 >= l2 else None
                if hdr is not None and is_likely_exe(hdr, exe_ranges):
                    result["findings"].append(f"  SLOT_VAL={slot_val:#x} is BASE object (TypeInfo in exe)")
                    result["slot_verdict"] = "SLOT_POINTS_TO_BASE"
                elif hdr_m16 is not None and is_likely_exe(hdr_m16, exe_ranges):
                    result["findings"].append(
                        f"  SLOT_VAL={slot_val:#x} is INTERIOR+16 truebase={slot_val-16:#x} tip={hdr_m16:#x}")
                    result["slot_verdict"] = "SLOT_HOLDS_INTERIOR_PLUS16"
                else:
                    result["findings"].append(
                        f"  SLOT_VAL={slot_val:#x} hdr={hdr} hdr-16={hdr_m16}")
                    result["slot_verdict"] = "SLOT_UNCLASSIFIED"
            else:
                result["findings"].append(f"  SLOT_VAL={slot_val:#x} not in any dump map")
                result["slot_verdict"] = "SLOT_NO_DUMP"

    # For SIZEGUARD: classic geometry is reading obj as base when it's interior
    if sg:
        obj = sg["obj"]
        mf = find_mem_file(run_dir, obj)
        if mf:
            tag,lo,hi,path = mf
            # instanceSize at TypeInfo+? — for B2, *obj is code bytes misread as size
            at = read_u64(path, lo, obj)
            before = read_u64(path, lo, obj-16) if obj-16 >= lo else None
            if before is not None and is_likely_exe(before, exe_ranges) and at is not None and is_likely_exe(at, exe_ranges):
                result["geometry"] = "B2_CLASSIC_OBJ_IS_INTERIOR_PLUS16"
            elif at is not None and is_likely_exe(at, exe_ranges):
                result["geometry"] = "OBJ_HAS_EXE_HEADER_NOT_INTERIOR"
            else:
                result["geometry"] = f"OBJ_HEADER_at={at:#x if at else None}_before={before:#x if before else None}"
            # Who holds a ref to this obj? scan holder candidates from stderr region walk not available;
            # scan remset-ish: look at nearby objects' +0x10 slots for value==obj
            # (bounded scan of same region)
            rstart, rend = sg["rstart"], sg["rend"]
            mf_r = find_mem_file(run_dir, rstart)
            holders = []
            if mf_r:
                _, rlo, rhi, rpath = mf_r
                # scan every 8B in region for value == obj or obj-related
                scan_lo = max(rlo, rstart)
                scan_hi = min(rhi, rend)
                with open(rpath,'rb') as f:
                    f.seek(scan_lo - rlo)
                    data = f.read(scan_hi - scan_lo)
                for i in range(0, len(data)-8, 8):
                    v = struct.unpack_from('<Q', data, i)[0]
                    if v == obj or v == (obj - 16) or v == (obj + 16):
                        holders.append((scan_lo+i, v))
                result["findings"].append(f"region_scan holders_of_obj_or_±16: n={len(holders)} sample={holders[:8]}")
    return result

def main():
    runs = sorted(glob.glob(os.path.join(ROOT, "runs", "d2_r*")))
    if not runs:
        runs = sorted(glob.glob(os.path.join(ROOT, "runs", "d_r*")))
    print(f"ANALYZING {len(runs)} runs under {ROOT}")
    verdicts = {}
    for rd in runs:
        # skip if no fatal
        if not os.path.exists(os.path.join(rd,"supervisor.log")): continue
        sup = open(os.path.join(rd,"supervisor.log")).read()
        if "FATAL_SIGNAL" not in sup and "sizeguard" not in open(os.path.join(rd,"stderr.txt")).read() if os.path.exists(os.path.join(rd,"stderr.txt")) else True:
            # still try if stderr has family
            err = open(os.path.join(rd,"stderr.txt")).read() if os.path.exists(os.path.join(rd,"stderr.txt")) else ""
            if not re.search(r'sizeguard|IsValidObject|toRegion2|IsMarked|SIGSEGV|Invalid object', err):
                continue
        r = analyze_run(rd)
        print("="*60)
        print(r["run"])
        if r.get("geometry"): print(" geometry:", r["geometry"])
        if r.get("slot_verdict"): print(" slot_verdict:", r["slot_verdict"])
        for f in r["findings"]:
            print(" ", f)
        key = r.get("slot_verdict") or r.get("geometry") or "OTHER"
        verdicts[key] = verdicts.get(key,0)+1
    print("="*60)
    print("VERDICT_HIST", verdicts)

if __name__ == "__main__":
    main()
