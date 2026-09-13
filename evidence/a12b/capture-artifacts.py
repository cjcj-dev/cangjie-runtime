import json, pathlib, subprocess, hashlib
root=pathlib.Path("/root/sym_cangjie_runtime_495_implement_r5656147173")
result={"source_sha256":hashlib.sha256((root/"source.tar.gz").read_bytes()).hexdigest(),"nproc":subprocess.check_output(["nproc"],text=True).strip(),"uptime_before":(root/"uptime-before.txt").read_text(),"uptime_after":(root/"uptime-after.txt").read_text(),"arms":{}}
for arm in ["default","testable"]:
    so=root/arm/"build/runtime-staging/lib/x86_64_Release/libcangjie-runtime.so"
    nm=subprocess.run(["nm","--defined-only",str(so)],capture_output=True,text=True)
    (root/(arm+"-nm-defined.txt")).write_text(nm.stdout)
    cache=(root/arm/"build/CMakeCache.txt").read_text().splitlines()
    result["arms"][arm]={"configure_rc":(root/(arm+"-configure.rc")).read_text().strip(),"build_rc":(root/(arm+"-build.rc")).read_text().strip(),"wall":(root/(arm+"-wall.txt")).read_text().strip(),"captured_so_hashes":(root/(arm+"-so.sha256")).read_text(),"nm_rc":nm.returncode,"nm_matches":[x for x in nm.stdout.splitlines() if any(s in x for s in ["ComputeMemoryUsageInfo","GCIdMark","ZGCIdPrinter"])],"config":[x for x in cache if any(x.startswith(v) for v in ["MRT_TESTABLE_INTERNALS:","CJ_RUNTIME_COMMIT:","CMAKE_BUILD_TYPE:"])]}
files=["gclog_schema.py","test_gclog_schema.py","test_phase_leaf_ledger.py","test_phase_entry_guard.py","test_analyze_youngstw.py"]
for f in files:
    path=root/"default/runtime/tests/perf_vs_official"/f
    compile(path.read_text(),str(path),"exec")
result["python_syntax"]={"files":files,"rc":0,"executed_tests":False}
(root/"artifact-metadata.json").write_text(json.dumps(result,indent=2)+"\n")
print(json.dumps(result,indent=2))

