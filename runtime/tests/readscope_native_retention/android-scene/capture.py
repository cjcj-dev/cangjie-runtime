import json
import os
import sys

CAP = 2048
BOUNDARY_CAP = 64

def fresh():
    return {
        "stacks": {},
        "dead": {},
        "generations": {},
        "events": [],
        "boundaries": [],
        "missing": [],
        "truncated": False,
        "complete": True,
        "fatal": None,
        "chain": "UNKNOWN",
    }

def on_call(state, tid, kind, mutex, pc, module):
    if state["truncated"] or len(state["events"]) >= CAP:
        state["truncated"] = True
        state["complete"] = False
        return state
    state["events"].append({
        "phase": "entry", "tid": tid, "kind": kind, "mutex": mutex, "pc": pc, "module": module,
    })
    state["stacks"].setdefault(str(tid), []).append(state["events"][-1])
    return state

def on_return(state, tid, rax, pc, module):
    if state["truncated"]:
        state["complete"] = False
        return state
    stack = state["stacks"].setdefault(str(tid), [])
    if not stack:
        state["events"].append({"phase": "unpaired-return", "tid": tid, "return": rax, "pc": pc, "module": module})
        state["complete"] = False
        return state
    call = stack.pop()
    event = {
        "phase": "return", "tid": tid, "kind": call["kind"], "mutex": call["mutex"],
        "return": rax, "entryPc": call["pc"], "entryModule": call["module"], "pc": pc, "module": module,
    }
    state["events"].append(event)
    if rax != 0 or call["mutex"] is None:
        return state
    key = str(call["mutex"])
    previous = state["generations"].get(key, {"gen": 0})
    generation = {"gen": previous["gen"] + 1, "state": "live" if call["kind"] == "init" else "dead", "event": event}
    state["generations"][key] = generation
    if call["kind"] == "destroy":
        state["dead"][key] = generation
    elif call["kind"] == "init":
        state["dead"].pop(key, None)
    return state

def associate(state, dwarf_mutex, message_address):
    if not state["complete"]:
        return {"chain": "UNKNOWN", "address": None, "source": "truncated", "sceneSuccess": False}
    for source, value in (("dwarf", dwarf_mutex), ("fatal-message", message_address)):
        if value is None:
            continue
        row = state["dead"].get(str(value))
        if row and row.get("state") == "dead":
            return {"chain": "DESTROY0_THEN_LOCK", "address": value, "source": source, "sceneSuccess": False}
    return {"chain": "UNKNOWN", "address": None, "source": "argument-not-recovered", "sceneSuccess": False}

def out_path():
    root = os.environ.get("EXIT_SCENE_OUT")
    if not root:
        raise RuntimeError("EXIT_SCENE_OUT is unset")
    return os.path.join(root, "mutex-events.json")

def load():
    path = out_path()
    if not os.path.exists(path):
        return fresh()
    with open(path) as handle:
        return json.load(handle)

def save(state):
    path = out_path()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    temporary = path + ".tmp"
    with open(temporary, "w") as handle:
        json.dump(state, handle, indent=2)
        handle.write("\n")
    os.replace(temporary, path)

def frame_name(frame):
    if frame is None or not frame.IsValid():
        return None
    return frame.GetFunctionName() or None

def reg(frame, name):
    import lldb
    value = frame.FindRegister(name)
    if not value.IsValid():
        return None
    error = lldb.SBError()
    number = value.GetValueAsUnsigned(error)
    if error.Fail():
        return None
    return number

def module_of(frame):
    module = frame.GetModule()
    if not module.IsValid():
        return None
    return module.GetFileSpec().fullpath

def dwarf_mutex(frame):
    variables = frame.GetVariables(True, True, False, True)
    for value in variables:
        if value.GetName() not in ("mutex", "__mutex"):
            continue
        import lldb
        error = lldb.SBError()
        number = value.GetValueAsUnsigned(error)
        if not error.Fail():
            return number
    return None

def arm():
    import lldb
    target = lldb.debugger.GetSelectedTarget()
    if not target.IsValid():
        raise RuntimeError("no target")
    names = [
        ("pthread_mutex_init", "capture.on_init"),
        ("pthread_mutex_destroy", "capture.on_destroy"),
        ("FiniCJRuntime", "capture.on_fini"),
        ("ScheduleStopOutside", "capture.on_stop_outside"),
        ("CJ_ScheduleThreadFini", "capture.on_thread_fini"),
        ("MRT_LibraryUnLoad", "capture.on_unload"),
    ]
    state = load()
    for name, callback in names:
        breakpoint = target.BreakpointCreateByName(name)
        breakpoint.SetScriptCallbackFunction(callback)
        if breakpoint.GetNumLocations() == 0:
            state["missing"].append(name)
    save(state)

def remember(kind, frame):
    state = load()
    on_call(state, frame.GetThread().GetThreadID(), kind, reg(frame, "rdi"), frame.GetPC(), module_of(frame))
    save(state)
    if not state["truncated"]:
        frame.GetThread().StepOut()
    return False

def on_init(frame, location, extra, internal):
    if "pthread_mutex_init" in (frame_name(frame) or ""):
        return remember("init", frame)
    return False

def on_destroy(frame, location, extra, internal):
    if "pthread_mutex_destroy" in (frame_name(frame) or ""):
        return remember("destroy", frame)
    return False

def boundary(kind, frame):
    state = load()
    if len(state["boundaries"]) >= BOUNDARY_CAP:
        state["truncated"] = True
        state["complete"] = False
    else:
        state["boundaries"].append({
            "kind": kind, "name": frame_name(frame),
            "tid": frame.GetThread().GetThreadID() if frame.IsValid() else None,
            "pc": frame.GetPC() if frame.IsValid() else None,
            "module": module_of(frame) if frame.IsValid() else None,
        })
    save(state)
    return False

def on_fini(frame, location, extra, internal):
    return boundary("FiniCJRuntime", frame)

def on_stop_outside(frame, location, extra, internal):
    return boundary("ScheduleStopOutside", frame)

def on_thread_fini(frame, location, extra, internal):
    return boundary("CJ_ScheduleThreadFini", frame)

def on_unload(frame, location, extra, internal):
    return boundary("MRT_LibraryUnLoad", frame)

def stacks(process):
    rows = []
    for thread in process:
        frames = []
        for frame in thread:
            if len(frames) >= 24:
                break
            frames.append({"pc": frame.GetPC(), "name": frame_name(frame), "module": module_of(frame)})
        rows.append({"tid": thread.GetThreadID(), "frames": frames})
    return rows

def on_stop():
    import lldb
    process = lldb.debugger.GetSelectedTarget().GetProcess()
    if not process.IsValid():
        return
    thread = process.GetSelectedThread()
    state = load()
    reason = thread.GetStopReason()
    if reason == lldb.eStopReasonPlanComplete:
        frame = thread.GetSelectedFrame()
        on_return(state, thread.GetThreadID(), reg(frame, "rax"), frame.GetPC(), module_of(frame))
        save(state)
        process.Continue()
        return
    if reason != lldb.eStopReasonSignal or state.get("fatal"):
        return
    recovered = None
    for frame in thread:
        if "pthread_mutex_lock" in (frame_name(frame) or ""):
            recovered = dwarf_mutex(frame)
            break
    linked = associate(state, recovered, None)
    state["fatal"] = {
        "tid": thread.GetThreadID(),
        "address": linked["address"],
        "source": linked["source"],
        "threads": stacks(process),
    }
    state["chain"] = linked["chain"]
    save(state)

def self_test():
    state = fresh()
    on_call(state, 1, "destroy", 10, 100, "libc.so")
    on_call(state, 2, "destroy", 20, 200, "libc.so")
    on_return(state, 2, 0, 201, "libc.so")
    on_return(state, 1, 0, 101, "libc.so")
    if state["dead"]["20"]["event"]["tid"] != 2 or state["dead"]["10"]["event"]["tid"] != 1:
        raise SystemExit("concurrent pairing mixed threads")
    nested = fresh()
    on_call(nested, 7, "destroy", 1, 1, "a")
    on_call(nested, 7, "destroy", 2, 2, "a")
    on_return(nested, 7, 0, 3, "b")
    on_return(nested, 7, 5, 4, "b")
    if "2" not in nested["dead"] or "1" in nested["dead"]:
        raise SystemExit("nested return did not bind the inner call")
    rejected = associate(state, None, None)
    if rejected["chain"] != "UNKNOWN":
        raise SystemExit("raw register was accepted")
    matched = associate(state, None, 10)
    if matched["chain"] != "DESTROY0_THEN_LOCK" or matched["sceneSuccess"]:
        raise SystemExit("message match failed")
    state["complete"] = False
    if associate(state, None, 10)["chain"] != "UNKNOWN":
        raise SystemExit("truncated state still claimed a chain")
    print("PAIRING_PASS")

if __name__ == "__main__" and len(sys.argv) > 1 and sys.argv[1] == "self-test":
    self_test()
