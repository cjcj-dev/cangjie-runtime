# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
# Load with gdb -x; external observation only, no inferior function calls.
# NOT_RUN until a controller authorizes the bounded A2 execution batch.
import hashlib
import json
import os
import gdb

observations = {"analyse": [], "resolve": [], "errors": []}
expected_hash = os.environ.get("A2_OBSERVER_SO_SHA256", "")
route = os.environ.get("A2_OBSERVER_ROUTE", "")
output = os.environ.get("A2_OBSERVER_OUT", "")


def product_frame():
    frame = gdb.newest_frame()
    symbol = frame.function()
    if symbol is None or symbol.symtab is None:
        raise RuntimeError("product debug ownership unavailable")
    path = symbol.symtab.objfile.filename
    if os.path.basename(path) != "libcangjie-runtime.so":
        raise RuntimeError("breakpoint is outside product SO: " + path)
    with open(path, "rb") as stream:
        actual = hashlib.sha256(stream.read()).hexdigest()
    if len(expected_hash) != 64 or actual != expected_hash:
        raise RuntimeError("actual breakpoint product hash mismatch")
    # The debugger's object must also be the mapped object, not a separate copy.
    pid = gdb.selected_inferior().pid
    with open("/proc/%d/maps" % pid) as stream:
        mapped = any(line.rstrip().endswith(path) for line in stream)
    if not mapped:
        raise RuntimeError("debug object not bound to inferior mapping")
    return frame, path, actual


class Returned(gdb.FinishBreakpoint):
    def __init__(self, frame, kind, record, context=None):
        super().__init__(frame, internal=True)
        self.kind, self.record, self.context = kind, record, context

    def stop(self):
        try:
            if self.kind == "resolve":
                if self.return_value is None:
                    raise RuntimeError("Resolve return value unavailable")
                self.record["accepted"] = int(self.return_value)
            else:
                self.record["type"] = str(self.context["frameInfo"]["fType"])
            self.record["returned"] = True
        except Exception as error:
            observations["errors"].append(str(error))
        return False

    def out_of_scope(self):
        observations["errors"].append(self.kind + " did not return normally")


class Entered(gdb.Breakpoint):
    def __init__(self, name, kind):
        super().__init__(name, internal=True)
        self.kind = kind

    def stop(self):
        try:
            frame, path, actual = product_frame()
            context = frame.read_var("uwContext") if self.kind == "analyse" else None
            info = context["frameInfo"] if context is not None else frame.read_var("this").dereference()
            record = {"ip": int(info["mFrame"]["ip"]), "so": path,
                      "sha256": actual, "returned": False}
            observations[self.kind].append(record)
            Returned(frame, self.kind, record, context)
        except Exception as error:
            observations["errors"].append(str(error))
        return False


def exited(event):
    observations["inferior_rc"] = getattr(event, "exit_code", None)
    analyse, resolve = observations["analyse"], observations["resolve"]
    native = len(analyse) == 1 and analyse[0].get("returned") and analyse[0].get("type", "").endswith("NATIVE")
    same_input = len(resolve) == 1 and len(analyse) == 1 and resolve[0]["ip"] == analyse[0]["ip"]
    valid_resolve = same_input and resolve[0].get("returned") and resolve[0].get("accepted") == 0
    observations["target"] = bool(not observations["errors"] and observations["inferior_rc"] == 0 and native and
                                  ((route == "direct" and not resolve) or (route == "resolve" and valid_resolve)))
    with open(output, "w") as stream:
        json.dump(observations, stream, indent=2)
    gdb.write("A2_OWNER_ROUTE_TARGET " + json.dumps(observations) + "\n")
    gdb.execute("quit %d" % (0 if observations["target"] else 1))


if route not in ("direct", "resolve") or len(expected_hash) != 64 or not output:
    gdb.write("A2_OWNER_ROUTE_INPUT_MISSING\n")
    gdb.execute("quit 2")
else:
    # Load the real dynamic libraries once, stopping before main's test input.
    gdb.execute("start")
    executable = gdb.current_progspace().filename
    with open(executable, "rb") as stream:
        observations["elf_sha256"] = hashlib.sha256(stream.read()).hexdigest()
    observations["elf"] = executable
    gdb.execute("set breakpoint pending off")
    Entered("MapleRuntime::StackFrameStream::AnalyseAndSetFrameType", "analyse")
    Entered("MapleRuntime::FrameInfo::ResolveProcInfo", "resolve")
    gdb.events.exited.connect(exited)
    gdb.execute("continue")
