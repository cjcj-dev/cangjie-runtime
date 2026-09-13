import json, subprocess
from pathlib import Path
root = Path(__file__).resolve().parents[2]
out = Path(__file__).resolve().parent

def run(args):
    p = subprocess.run(args, cwd=root, text=True, capture_output=True)
    return {"command": args, "rc": p.returncode, "stdout": p.stdout, "stderr": p.stderr}

baseline = "78fc9ce028de705b3ea705b7069759c1036a2796"
removed = [
    ("ActiveSeq|CycleCounter|BeginCycle|CompleteCycle", "runtime/src"),
    ("totalSize", "runtime/src/Inspector/ProfilerAgentImpl.cpp"),
    (r"class ZStatSamplerHistory|struct ZStatSamplerData \{", "runtime/src/Heap/z/zStat.hpp"),
    ("ZStat::SampleAndCollect|ZStat::Print", "runtime/src/Heap/z/zStat.cpp"),
]
checks=[]
for pattern, path in removed:
    checks.append({"pattern":pattern,"path":path,
        "baseline_positive":run(["git","grep","-n","-E",pattern,baseline,"--",path]),
        "candidate":run(["git","grep","-n","-E",pattern,"--",path])})
(out/"deletions.json").write_text(json.dumps(checks,indent=2)+"\n")
retained=[]
for symbol in ["ZYoungType::major_full_preclean","ZYoungType::major_full_roots","ZYoungType::major_partial_roots","CancelDriverRequestLifecycle","GetGenerationCycle","GetYoungDriverPort","StashSegments","RestoreSegments"]:
    retained.append({"symbol":symbol,"main":run(["git","grep","-c","-F",symbol,"cjcjdev/main","--","runtime/src"]),
        "candidate":run(["git","grep","-c","-F",symbol,"HEAD","--","runtime/src"])})
for row in retained:
    for arm in ["main", "candidate"]:
        row["total_" + arm] = sum(int(line.rsplit(":", 1)[1]) for line in row[arm]["stdout"].splitlines())
    row["retained"] = row["total_candidate"] >= row["total_main"]
(out/"main-content.json").write_text(json.dumps(retained,indent=2)+"\n")
(out/"source-state.json").write_text(json.dumps({"head":run(["git","rev-parse","HEAD"]),"main":run(["git","rev-parse","cjcjdev/main"]),"diff":run(["git","diff","--stat","cjcjdev/main"]),"diff_check":run(["git","diff","--check"]),"conflicts":run(["git","grep","-n","-E","^(<<<<<<< |=======|>>>>>>> )","--","runtime/src"])},indent=2)+"\n")
