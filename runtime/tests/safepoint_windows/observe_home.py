"""Read the live Win64 home/save boundaries at exact product callee entries.

Run by native GDB after `start`. No inferior function calls, register writes,
or stack writes. HotSpot sharedRuntime_x86_64.cpp:273-275 reserves the home
area after saving registers. The CALL return address is below that area.
"""
import hashlib
import json
import os
from pathlib import Path

import gdb


names = ("MRT_UpdateUwContext", "HandleSafepoint", "MRT_GetThreadLocalData",
         "MRT_DeleteC2NContext")
records = []
result = {"entries": records}
inferior = gdb.selected_inferior()


def read(address, size):
    return bytes(inferior.read_memory(address, size))


def address(name):
    return int(gdb.parse_and_eval("(unsigned long long) &" + name))


class Entry(gdb.Breakpoint):
    def __init__(self, name, target, return_pc):
        super().__init__("*" + hex(target), internal=True)
        self.name = name
        self.return_pc = return_pc

    def stop(self):
        frame = gdb.newest_frame()
        rsp, rbp = (int(frame.read_register(r)) for r in ("rsp", "rbp"))
        return_pc = int.from_bytes(read(rsp, 8), "little")
        if return_pc != self.return_pc:
            return False  # An unrelated caller is not an observation of this stub.
        # At the first instruction of the callee, RSP points to CALL's return
        # address; the caller's 32-byte home area starts eight bytes above it.
        home_lo, home_hi = rsp + 8, rsp + 8 + 32
        saved_lo, saved_hi = rbp - 368, rbp
        disjoint = home_hi <= saved_lo or saved_hi <= home_lo
        saved = read(rbp - 368, 32)
        expected = (0x55aa33cc77ee1199, 0x1122446688aaccee,
                    0x123456789abcdef0, 0xfedcba9876543210)
        frame_valid = saved == b"".join(n.to_bytes(8, "little") for n in expected)
        record = dict(callee=self.name, pc=frame.pc(), rsp=rsp, rbp=rbp,
                      return_pc=return_pc, home=[home_lo, home_hi],
                      saved=[saved_lo, saved_hi], saved_simd=saved.hex(),
                      frame_valid=frame_valid, passed=disjoint)
        records.append(record)
        print("HOME_TARGET " + json.dumps(record, sort_keys=True), flush=True)
        return False


try:
    stub = address("CJ_MCC_HandleSafepoint")
    # GDB can resolve the EXE's import thunk rather than the DLL's definition.
    # Follow that actual IAT jump, then bind all observations to this DLL.
    if read(stub, 2) == b"\xff\x25":
        slot = stub + 6 + int.from_bytes(read(stub + 2, 4), "little", signed=True)
        stub = int.from_bytes(read(slot, 8), "little")
    module = Path(gdb.solib_name(stub))
    expected_module = Path(os.environ["SAFEPOINT_PRODUCT"])
    if module.resolve() != expected_module.resolve():
        raise RuntimeError("unexpected loaded product: " + str(module))
    result.update(module=str(module), dll_sha256=hashlib.sha256(module.read_bytes()).hexdigest(),
                  stub=stub)
    calls = []
    for instruction in gdb.newest_frame().architecture().disassemble(stub, count=160):
        pc, size = instruction["addr"], instruction["length"]
        code = read(pc, size)
        if code[0] == 0xe8:
            calls.append((pc + size + int.from_bytes(code[1:], "little", signed=True), pc + size))
        if instruction["asm"].startswith("ret"):
            break
    if len(calls) != len(names):
        raise RuntimeError("ordinary stub call sequence changed: " + repr(calls))
    breakpoints = []
    for name, (target, return_pc) in zip(names, calls):
        if target != address(name):
            raise RuntimeError("ordinary stub target mismatch: " + name)
        breakpoints.append(Entry(name, target, return_pc))
    gdb.execute("continue")
    result["inferior_rc"] = int(gdb.parse_and_eval("$_exitcode"))
    complete = [r["callee"] for r in records] == list(names)
    result["complete"] = complete
    rc = 0 if complete and all(r["frame_valid"] and r["passed"] for r in records) else 1
except Exception as error:
    result["error"] = str(error)
    rc = 2
result["rc"] = rc
Path(os.environ["SAFEPOINT_HOME_RESULT"]).write_text(json.dumps(result, indent=2))
print("HOME_RESULT " + json.dumps(result, sort_keys=True), flush=True)
gdb.execute("quit " + str(rc))
