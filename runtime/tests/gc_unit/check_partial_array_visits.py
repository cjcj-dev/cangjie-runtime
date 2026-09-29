#!/usr/bin/env python3
"""Observe product array visits without recompiling or instrumenting the SO.

ZGC zMark.cpp:198-214,234-254: every input slot belongs to exactly one visit.
Run after run_standalone.sh with its ELF and product library directory. Requires
Linux gdb with Python; no product callbacks, counters or replacement symbols.
"""
import os
import sys


def observe():
    import gdb

    state = {"active": False, "runs": 0, "failed": False, "drains": 0}

    def register(name):
        return int(gdb.parse_and_eval("$" + name))

    class Completed(gdb.FinishBreakpoint):
        def stop(self):
            counts = state["counts"]
            missing = sum(value == 0 for value in counts)
            repeated = sum(value > 1 for value in counts)
            ok = not (missing or repeated or state["outside"] or state["unaligned"])
            state["failed"] |= not ok
            state["runs"] += 1
            print("ARRAY_VISITS_TARGET entries=%d visits=%d missing=%d repeated=%d "
                  "outside=%d unaligned=%d drains=%d verdict=%s" %
                  (len(counts), sum(counts), missing, repeated, state["outside"],
                   state["unaligned"], state["drains"], "PASS" if ok else "FAIL"), flush=True)
            state["active"] = False
            return False

        def out_of_scope(self):
            state["failed"] = True
            state["active"] = False
            print("ARRAY_VISITS_TARGET FAIL OnSet did not return", flush=True)

    class Begin(gdb.Breakpoint):
        def stop(self):
            state.update(active=True, start=int(gdb.parse_and_eval("addr")),
                         counts=[0] * int(gdb.parse_and_eval("length")),
                         outside=0, unaligned=0, drains=0)
            Completed(gdb.newest_frame(), internal=True)
            return False

    class Drain(gdb.Breakpoint):
        def stop(self):
            if state["active"]:
                state["drains"] += 1
            return False

    class Visit(gdb.Breakpoint):
        def stop(self):
            if not state["active"]:
                return False
            # The existing field barrier is out-of-line even when small/large
            # followers are inlined. Identify the actual array caller, excluding
            # the ordinary object fields reached after marking array elements.
            frame = gdb.newest_frame().older()
            array = False
            while frame is not None:
                name = frame.name() or ""
                if "follow_array" in name or "mark_barrier_on_oop_array" in name:
                    array = True
                    break
                if any(part in name for part in ("follow_object", "ZMarkBarrierFollowOopClosure", "ZIterator::")):
                    break
                frame = frame.older()
            if not array:
                return False
            arch = gdb.newest_frame().architecture().name()
            slot = register("x0" if "aarch64" in arch else "rdi")
            delta = slot - state["start"]
            if delta < 0 or delta >= len(state["counts"]) * 8:
                state["outside"] += 1
            elif delta % 8:
                state["unaligned"] += 1
            else:
                state["counts"][delta // 8] += 1
            return False

    gdb.execute("set pagination off")
    gdb.execute("set confirm off")
    gdb.execute("set breakpoint pending on")
    # Catch Runtime SO loading before resolving exact instruction addresses.
    gdb.execute("start")
    Begin("(anonymous namespace)::OnSet", internal=True)
    Drain("MapleRuntime::ZMark::Drain", internal=True)
    # Break at the first instruction, before the ABI argument register changes.
    for symbol in os.environ["ARRAY_VISIT_SYMBOLS"].split(":"):
        Visit("*" + symbol, internal=True)
    gdb.execute("continue")
    try:
        inferior_rc = int(gdb.parse_and_eval("$_exitcode"))
    except gdb.error:
        inferior_rc = -1
    ok = inferior_rc == 0 and state["runs"] > 0 and not state["failed"] and not state["active"]
    print("ARRAY_VISITS_RESULT runs=%d inferior_rc=%d rc=%d" %
          (state["runs"], inferior_rc, 0 if ok else 1), flush=True)
    gdb.execute("quit %d" % (0 if ok else 1))


def main():
    import argparse
    import hashlib
    import pathlib
    import subprocess

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=pathlib.Path)
    parser.add_argument("library_dir", type=pathlib.Path)
    parser.add_argument("--filter", default="PartialArray.MultiChunk")
    args = parser.parse_args()
    elf = args.elf.resolve()
    so = args.library_dir.resolve() / "libcangjie-runtime.so"
    product = subprocess.check_output(["nm", "--defined-only", str(so)], text=True)
    local = subprocess.check_output(["nm", "--defined-only", str(elf)], text=True)
    symbols = [line.split()[-1] for line in product.splitlines()
               if "MarkBarrierOnOldOopField" in line and "HeapSlot" in line]
    if len(symbols) != 1 or any(symbol in local for symbol in symbols):
        raise RuntimeError("array visit observer requires exactly one product barrier, no ELF copy")
    for path in (elf, so, so.with_name("libboundscheck.so")):
        print("ARRAY_VISITS_IDENTITY sha256=%s file=%s" %
              (hashlib.sha256(path.read_bytes()).hexdigest(), path), flush=True)
    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = str(args.library_dir.resolve())
    env["ARRAY_VISIT_SYMBOLS"] = ":".join(symbols)
    # Directly execute the selected other-vm body, retaining the runner's
    # existing child contract (no forked inferior hidden from gdb).
    if args.filter == "PartialArray.BoundaryRefs":
        env["GC_UNIT_OTHER_VM_CHILD"] = args.filter
    command = ["gdb", "-q", "-nx", "-batch", "-x", str(pathlib.Path(__file__).resolve()),
               "--args", str(elf), "--gtest_filter=" + args.filter]
    return subprocess.run(command, env=env, timeout=180).returncode


try:
    import gdb
except ImportError:
    if __name__ == "__main__":
        sys.exit(main())
else:
    observe()
