import json
import sys

CAP = 2048
BOUNDARY_CAP = 64
REMOTE_RUNTIME = '/data/local/tmp/readscope-lifetime/libcangjie-runtime.so'
REMOTE_CONSUMER = '/data/local/tmp/readscope-lifetime/consumer'

def fresh():
    return {
        "stacks": {}, "dead": {}, "generations": {}, "events": [], "boundaries": [],
        "missing": [], "truncated": False, "complete": True, "fatal": None, "chain": "UNKNOWN",
    }

def stack_ok(frames):
    return isinstance(frames, list) and len(frames) >= 2 and all(
        isinstance(frame, dict) and frame.get("pc") is not None and frame.get("module") and frame.get("name")
        for frame in frames)

def on_call(state, tid, kind, mutex, frames):
    if state["truncated"] or len(state["events"]) >= CAP:
        state["truncated"] = True
        state["complete"] = False
        return state
    if not stack_ok(frames):
        state["complete"] = False
    state["events"].append({
        "phase": "entry", "tid": tid, "kind": kind, "mutex": mutex, "frames": frames or [],
        "pc": (frames or [{}])[0].get("pc"), "module": (frames or [{}])[0].get("module"),
    })
    state["stacks"].setdefault(str(tid), []).append(state["events"][-1])
    return state

def on_return(state, tid, rax, frames):
    if state["truncated"]:
        state["complete"] = False
        return state
    stack = state["stacks"].setdefault(str(tid), [])
    if not stack:
        state["events"].append({"phase": "unpaired-return", "tid": tid, "return": rax, "frames": frames or []})
        state["complete"] = False
        return state
    call = stack.pop()
    event = {
        "phase": "return", "tid": tid, "kind": call["kind"], "mutex": call["mutex"], "return": rax,
        "frames": call.get("frames") or [], "returnFrames": frames or [],
        "pc": call.get("pc"), "module": call.get("module"),
    }
    state["events"].append(event)
    if not stack_ok(event["frames"]):
        state["complete"] = False
        return state
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

def unknown(reason):
    return {"chain": "UNKNOWN", "address": None, "source": reason, "sceneSuccess": False}

def note_coverage(state, name, final):
    state.setdefault("coverage", {})[name] = final
    if final in ("bound", "hit"):
        state["missing"] = [item for item in state.get("missing", []) if item != name]
    return state

def resolve_coverage(state, counts):
    for name, count in counts.items():
        if (state.get("coverage") or {}).get(name) == "hit":
            note_coverage(state, name, "hit")
            continue
        if count:
            note_coverage(state, name, "bound")
    return state

def associate(state, dwarf_mutex, message_address, fatal_frames=None):
    if "pthread_mutex_init" in state.get("missing", []) or (state.get("coverage") or {}).get("pthread_mutex_init") == "pending":
        return unknown("init-coverage")
    if not state.get("complete", False):
        return unknown("truncated")
    if not stack_ok(fatal_frames):
        return unknown("fatal-stack")
    for source, value in (("dwarf", dwarf_mutex), ("fatal-message", message_address)):
        if value is None:
            continue
        row = state.get("dead", {}).get(str(value))
        if not row or row.get("state") != "dead":
            continue
        if not stack_ok((row.get("event") or {}).get("frames")):
            return unknown("destroy-stack")
        return {"chain": "DESTROY0_THEN_LOCK", "address": value, "source": source, "sceneSuccess": False,
                "destroy": row["event"], "fatalFrames": fatal_frames}
    return unknown("argument-not-recovered")

def out_path():
    import os
    root = os.environ.get("EXIT_SCENE_OUT")
    if not root:
        raise RuntimeError("EXIT_SCENE_OUT is unset")
    return os.path.join(root, "mutex-events.json")

def load():
    import json as json_mod
    import os
    path = out_path()
    if not os.path.exists(path):
        return fresh()
    with open(path) as handle:
        return json_mod.load(handle)

def save(state):
    import os
    path = out_path()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    temporary = path + ".tmp"
    with open(temporary, "w") as handle:
        json.dump(state, handle)
        handle.write("\n")
    os.replace(temporary, path)

def frame_name(frame):
    if frame is None or not frame.IsValid():
        return None
    return frame.GetFunctionName() or None

def module_of(frame):
    module = frame.GetModule()
    if not module.IsValid():
        return None
    return module.GetFileSpec().fullpath

def frames_of(thread):
    rows = []
    for frame in thread:
        if len(rows) >= 24:
            break
        rows.append({"pc": frame.GetPC(), "name": frame_name(frame), "module": module_of(frame)})
    return rows

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

def dwarf_mutex(frame):
    import lldb
    for value in frame.GetVariables(True, True, False, True):
        if value.GetName() not in ("mutex", "__mutex"):
            continue
        error = lldb.SBError()
        number = value.GetValueAsUnsigned(error)
        if not error.Fail():
            return number
    return None

def arm():
    import lldb
    target = lldb.debugger.GetSelectedTarget()
    if not target.IsValid():
        raise RuntimeError("arm before target create")
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
        locations = breakpoint.GetNumLocations()
        state.setdefault("breakpoints", {})[name] = breakpoint.GetID()
        if locations == 0:
            state["missing"].append(name)
            note_coverage(state, name, "pending")
        else:
            note_coverage(state, name, "bound")
    save(state)

def remember(kind, frame):
    state = load()
    on_call(state, frame.GetThread().GetThreadID(), kind, reg(frame, "rdi"), frames_of(frame.GetThread()))
    save(state)
    if not state["truncated"]:
        frame.GetThread().StepOut()
    return False

def on_init(frame, location, extra, internal):
    state = load()
    note_coverage(state, "pthread_mutex_init", "hit")
    save(state)
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
        state["boundaries"].append({"kind": kind, "tid": frame.GetThread().GetThreadID(), "frames": frames_of(frame.GetThread())})
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

EXAMINABLE = {'stopped', 'crashed', 'suspended'}

def examinable(state):
    return state in EXAMINABLE

def live_module():
    import lldb
    target = lldb.debugger.GetSelectedTarget()
    process = target.GetProcess()
    names = {
        lldb.eStateStopped: 'stopped', lldb.eStateCrashed: 'crashed', lldb.eStateSuspended: 'suspended',
        lldb.eStateExited: 'exited', lldb.eStateDetached: 'detached', lldb.eStateRunning: 'running',
    }
    name = names.get(process.GetState(), 'unknown')
    print(f'MODULE_LIVE pid={process.GetID()} state={name}', flush=True)
    if not process.IsValid() or not examinable(name):
        print('MODULE_UNAVAILABLE', flush=True)
        return
    platform = target.GetPlatform()
    maps = lldb.SBPlatformShellCommand(f'cat /proc/{process.GetID()}/maps')
    platform.Run(maps)
    print('MAPS_BEGIN', flush=True)
    print(maps.GetOutput() or '', flush=True)
    print('MAPS_END', flush=True)
    hashed = lldb.SBPlatformShellCommand(f'sha256sum {REMOTE_RUNTIME}')
    platform.Run(hashed)
    print('FILE_SHA ' + (hashed.GetOutput() or '').strip(), flush=True)
    print(target_line(process, platform), flush=True)

def dump():
    print('EVENT_JSON:' + json.dumps(load()), flush=True)

def seal():
    import lldb
    target = lldb.debugger.GetSelectedTarget()
    state = load()
    counts = {}
    for name, ident in (state.get("breakpoints") or {}).items():
        breakpoint = target.FindBreakpointByID(ident)
        counts[name] = breakpoint.GetNumLocations() if breakpoint.IsValid() else 0
    resolve_coverage(state, counts)
    save(state)
    dump()

def target_line(process, platform):
    import lldb
    command = lldb.SBPlatformShellCommand(
        "pid=%d; start=$(awk '{print $22}' /proc/$pid/stat 2>/dev/null); exe=$(readlink /proc/$pid/exe 2>/dev/null); echo TARGET_IDENTITY pid=$pid start=$start path=$exe" % process.GetID())
    platform.Run(command)
    return (command.GetOutput() or '').strip()

def mark_detach():
    import lldb
    print('DETACH_INTENT', flush=True)
    target = lldb.debugger.GetSelectedTarget()
    process = target.GetProcess() if target.IsValid() else None
    names = {lldb.eStateStopped: 'stopped', lldb.eStateCrashed: 'crashed', lldb.eStateSuspended: 'suspended'}
    if process and process.IsValid() and examinable(names.get(process.GetState(), '')):
        print(target_line(process, target.GetPlatform()), flush=True)
    else:
        print('TARGET_IDENTITY_ABSENT', flush=True)

def on_stop():
    import lldb
    target = lldb.debugger.GetSelectedTarget()
    if not target.IsValid():
        return
    process = target.GetProcess()
    if not process.IsValid():
        return
    thread = process.GetSelectedThread()
    state = load()
    if thread.GetStopReason() == lldb.eStopReasonPlanComplete:
        on_return(state, thread.GetThreadID(), reg(thread.GetSelectedFrame(), "rax"), frames_of(thread))
        save(state)
        process.Continue()
        return
    if thread.GetStopReason() != lldb.eStopReasonSignal or state.get("fatal"):
        return
    if thread.GetStopReasonDataAtIndex(0) not in (6, 11):
        return
    recovered = None
    for frame in thread:
        if "pthread_mutex_lock" in (frame_name(frame) or ""):
            recovered = dwarf_mutex(frame)
            break
    linked = associate(state, recovered, None, frames_of(thread))
    state["fatal"] = {"tid": thread.GetThreadID(), "address": linked["address"], "source": linked["source"], "frames": frames_of(thread)}
    state["chain"] = linked["chain"]
    save(state)

def two(name_a, name_b):
    return [{"pc": 1, "module": "libc.so", "name": name_a}, {"pc": 2, "module": "libconsumer.so", "name": name_b}]

def self_test():
    state = fresh()
    on_call(state, 1, "destroy", 10, two("pthread_mutex_destroy", "owner_a"))
    on_call(state, 2, "destroy", 20, two("pthread_mutex_destroy", "owner_b"))
    on_return(state, 2, 0, two("caller", "owner_b"))
    on_return(state, 1, 0, two("caller", "owner_a"))
    if state["dead"]["20"]["event"]["tid"] != 2 or state["dead"]["10"]["event"]["tid"] != 1:
        raise SystemExit("concurrent pairing mixed threads")
    nested = fresh()
    on_call(nested, 7, "destroy", 1, two("pthread_mutex_destroy", "outer"))
    on_call(nested, 7, "destroy", 2, two("pthread_mutex_destroy", "inner"))
    on_return(nested, 7, 0, two("mid", "inner"))
    on_return(nested, 7, 5, two("outer-ret", "outer"))
    if "2" not in nested["dead"] or "1" in nested["dead"]:
        raise SystemExit("nested return did not bind the inner call")
    if associate(state, None, None, two("pthread_mutex_lock", "user"))["chain"] != "UNKNOWN":
        raise SystemExit("missing address was accepted")
    bare = fresh()
    on_call(bare, 1, "destroy", 10, [{"pc": 1, "module": "libc.so"}])
    on_return(bare, 1, 0, two("caller", "user"))
    if associate(bare, None, 10, two("pthread_mutex_lock", "user"))["chain"] != "UNKNOWN":
        raise SystemExit("pc/module without a stack was accepted")
    matched = associate(state, None, 10, two("pthread_mutex_lock", "user"))
    if matched["chain"] != "DESTROY0_THEN_LOCK" or matched["sceneSuccess"] or not stack_ok(matched["destroy"]["frames"]):
        raise SystemExit("message match did not keep both stacks")
    state["complete"] = False
    if associate(state, None, 10, two("pthread_mutex_lock", "user"))["chain"] != "UNKNOWN":
        raise SystemExit("truncated state still claimed a chain")
    if examinable("exited") or examinable("detached") or examinable("running") or not examinable("stopped"):
        raise SystemExit("a dead or running process was treated as examinable")
    frames = two("pthread_mutex_destroy", "owner")
    positive = fresh()
    on_call(positive, 1, "destroy", 4660, frames)
    on_return(positive, 1, 0, frames)
    if associate(positive, 4660, None, two("pthread_mutex_lock", "user"))["chain"] != "DESTROY0_THEN_LOCK":
        raise SystemExit("observed coverage changed the independent control")
    uncovered = fresh()
    on_call(uncovered, 1, "destroy", 4660, frames)
    on_return(uncovered, 1, 0, frames)
    uncovered["missing"] = ["pthread_mutex_init"]
    if associate(uncovered, 4660, None, two("pthread_mutex_lock", "user"))["chain"] != "UNKNOWN":
        raise SystemExit("missing init coverage still claimed one generation")
    pending = fresh()
    on_call(pending, 1, "destroy", 4660, frames)
    on_return(pending, 1, 0, frames)
    note_coverage(pending, "pthread_mutex_init", "pending")
    pending["missing"] = ["pthread_mutex_init"]
    if associate(pending, 4660, None, two("pthread_mutex_lock", "user"))["chain"] != "UNKNOWN":
        raise SystemExit("pending init was treated as covered")
    resolve_coverage(pending, {"pthread_mutex_init": 1})
    if "pthread_mutex_init" in pending["missing"] or associate(pending, 4660, None, two("pthread_mutex_lock", "user"))["chain"] != "DESTROY0_THEN_LOCK":
        raise SystemExit("a later bind stayed missing")
    print("PAIRING_PASS")

if __name__ == "__main__" and len(sys.argv) > 1 and sys.argv[1] == "self-test":
    self_test()
elif __name__ == "__main__" and len(sys.argv) > 1 and sys.argv[1] == "decode":
    document = json.load(sys.stdin)
    print(json.dumps(associate(document["state"], document.get("dwarf"), document.get("messageAddress"), document.get("fatalFrames"))))
