#!/usr/bin/env python3
"""Rebuild only the product SO for #175's deterministic remap-window controls.

Run on the build host, in a private checkout with a completed testable build.
The test ELF is preserved once and reused byte-for-byte. Every run loads a
retained copy of runtime + boundscheck, hashes it before execution, and records
its exit code. Source mutations are temporary and restored in finally.
"""
import argparse
import difflib
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time

HEADER = "runtime/src/Heap/WCollector/WCollector.h"
PRODUCER = "runtime/src/Heap/Allocator/RegionManager.cpp"
ENTRY = "runtime/src/Heap/WCollector/WCollector.cpp"
COPY = "runtime/src/Heap/Collector/CopyCollector.cpp"
TESTS = (
    "YoungConc.KeptIdentityAtWaitRoutedGuard",
    "YoungConc.RemapWindowRealCopy",
    "ColourAddress.UncolorRoundTripAllRemapOneHot",
)
ARMS = ("reject", "accept", "cut_wait", "cut_publish", "cut_entry", "cut_copy", "restored", "reject_restored")


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def replace_one(text, before, after):
    if text.count(before) != 1:
        raise RuntimeError(f"expected one product anchor: {before!r}")
    return text.replace(before, after, 1)


def patch(before, after, name):
    return "".join(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                                        fromfile="a/" + name, tofile="b/" + name))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--elf", required=True, type=Path)
    parser.add_argument("--lib-dir", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--cores", required=True)
    parser.add_argument("--samples", type=int, default=3)
    parser.add_argument("--arms", default=",".join(ARMS))
    args = parser.parse_args()
    if args.samples < 1:
        parser.error("samples must be positive")
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    src = args.source.resolve()
    originals = {name: (src / name).read_text() for name in (HEADER, PRODUCER, ENTRY, COPY)}
    accepted = replace_one(originals[HEADER],
        "if (resolved != nullptr && resolved != obj)", "if (resolved != nullptr)")
    elf = out / "cj_gc_unit"
    shutil.copy2(args.elf, elf)
    manifest = {"elf_sha256": sha(elf), "source_commit": subprocess.check_output(
        ["git", "-C", str(src), "rev-parse", "HEAD"], text=True).strip(),
        "cores": args.cores, "samples": args.samples, "arms": {},
        "source_sha256_before": {name: sha(src / name) for name in originals}}
    report = out / "manifest.json"
    try:
        for arm in args.arms.split(","):
            if arm not in ARMS:
                raise RuntimeError(f"unknown arm {arm}")
            current = dict(originals)
            if arm not in ("reject", "reject_restored"):
                current[HEADER] = accepted
            cut = ""
            if arm == "cut_wait":
                current[HEADER] = replace_one(accepted,
                    "BaseObject* resolved = WaitRoutedTipReady(obj, to, forwarding, provenance);",
                    "BaseObject* resolved = nullptr;")
                cut = patch(accepted, current[HEADER], HEADER)
            elif arm == "cut_publish":
                before = '    (void)PublishKeptInPlaceReceipts(region);\n    VerifyForwardingReceiptsClosed(region, "FinishStayYoungInPlace");'
                current[PRODUCER] = replace_one(originals[PRODUCER], before,
                    '    (void)region;\n    VerifyForwardingReceiptsClosed(region, "FinishStayYoungInPlace");')
                cut = patch(originals[PRODUCER], current[PRODUCER], PRODUCER)
            elif arm == "cut_entry":
                current[ENTRY] = replace_one(originals[ENTRY],
                    "        DoYoungGarbageCollection();", "        (void)0;")
                cut = patch(originals[ENTRY], current[ENTRY], ENTRY)
            elif arm == "cut_copy":
                current[COPY] = replace_one(originals[COPY],
                    "        space.ForwardFromSpace<Generation::Young>(copyPool);", "        (void)copyPool;")
                cut = patch(originals[COPY], current[COPY], COPY)
            arm_dir = out / arm
            arm_dir.mkdir()
            (arm_dir / "cut.diff").write_text(cut)
            (arm_dir / "guard-control.diff").write_text(patch(originals[HEADER], accepted, HEADER)
                                                        if arm not in ("reject", "reject_restored") else "")
            for name, text in current.items():
                if (src / name).read_text() != text:
                    (src / name).write_text(text)
            env = os.environ.copy()
            env["GC_UNIT_GATE_SKIP"] = "1"
            start = time.time()
            with (arm_dir / "build.log").open("w") as log:
                built = subprocess.run(["taskset", "-c", args.cores, "cmake", "--build", str(args.build),
                    "--target", "cangjie-runtime", "-j16"], env=env, stdout=log, stderr=subprocess.STDOUT)
            record = {"build_rc": built.returncode, "build_started": start, "build_finished": time.time(),
                      "source_mtime": {name: (src / name).stat().st_mtime for name in originals}, "runs": []}
            manifest["arms"][arm] = record
            report.write_text(json.dumps(manifest, indent=2))
            if built.returncode:
                raise RuntimeError(f"{arm}: product build failed, rc={built.returncode}")
            for name in ("libcangjie-runtime.so", "libboundscheck.so"):
                shutil.copy2(args.lib_dir / name, arm_dir / name)
            record["sha256"] = {name: sha(arm_dir / name) for name in
                                ("libcangjie-runtime.so", "libboundscheck.so")}
            record["elf_sha256"] = sha(elf)
            strings = subprocess.check_output(["strings", str(arm_dir / "libcangjie-runtime.so")], text=True)
            record["lineage"] = re.findall(r"CJRT-COMMIT:\S+", strings)
            record["uptime_before"] = subprocess.check_output(["uptime"], text=True).strip()
            env["LD_LIBRARY_PATH"] = str(arm_dir)
            env["CJ_GC_UNIT_REMAP_WINDOW"] = "1"
            env.pop("GC_UNIT_OTHER_VM_CHILD", None)
            for sample in range(args.samples):
                for test in TESTS:
                    log_path = arm_dir / f"{test}.{sample}.log"
                    with log_path.open("w") as log:
                        run = subprocess.run(["taskset", "-c", args.cores, "timeout", "30", str(elf),
                                              "--gtest_filter=" + test], env=env, stdout=log, stderr=subprocess.STDOUT)
                    text = log_path.read_text()
                    is_control = test.startswith("ColourAddress.")
                    should_pass = is_control or arm in ("accept", "restored") or (
                        test == TESTS[1] and arm in ("reject", "reject_restored", "cut_wait"))
                    correct_rc = (run.returncode == 0) if should_pass else (run.returncode == 1)
                    details = {
                        "test": test, "sample": sample, "rc": run.returncode, "expected_pass": should_pass,
                        "expected_rc_matched": correct_rc, "log": str(log_path),
                        "started": f"[  RUN   ] {test}" in text,
                        "wait_entry": "REMAP_WINDOW wait_entry=1" in text,
                        "identity": "kept_identity=1 active_armed_hit=1 done=0 compacted=0" in text,
                        "guard_rejected": "ZRelocate::forward_object requires a forwarding entry" in text,
                        "guard_returned": "target_guard_returned_identity=1 done=0" in text,
                        "copy": "REMAP_WINDOW real_copy=1" in text,
                        "loaded_retained_so": f"REMAP_WINDOW product={arm_dir}/libcangjie-runtime.so" in text,
                        "single_test_tally": "[========] 1 tests:" in text,
                        "producer_gap": "FinishStayYoungInPlace receipt gap" in text,
                        "entry_missing": ("REMAP_WINDOW_TIMEOUT stage=flip-window" in text or
                                          "state.copied && state.published" in text),
                        "copy_gap": "lookup.answer == ForwardingTable::ToAnswer::ArmedHit" in text,
                    }
                    record["runs"].append(details)
                    print(f"{arm} {test} sample={sample} rc={run.returncode} expected={0 if should_pass else 1}", flush=True)
                    report.write_text(json.dumps(manifest, indent=2))
            record["uptime_after"] = subprocess.check_output(["uptime"], text=True).strip()
            report.write_text(json.dumps(manifest, indent=2))
    finally:
        for name, text in originals.items():
            if (src / name).read_text() != text:
                (src / name).write_text(text)
        manifest["source_sha256_after"] = {name: sha(src / name) for name in originals}
        report.write_text(json.dumps(manifest, indent=2))
    failed = [r for arm in manifest["arms"].values() for r in arm["runs"]
              if not r["started"] or not r["single_test_tally"] or not r["expected_rc_matched"]]
    for arm_name in ("reject", "reject_restored", "accept", "restored"):
        for r in manifest["arms"].get(arm_name, {}).get("runs", []):
            if r["test"] == TESTS[0]:
                if not (r["wait_entry"] and r["identity"] and r["loaded_retained_so"] and
                        (r["guard_rejected"] if arm_name.startswith("reject") else r["guard_returned"] and r["copy"])):
                    failed.append(r)
    for arm_name in ("cut_wait", "cut_publish", "cut_entry", "cut_copy"):
        for r in manifest["arms"].get(arm_name, {}).get("runs", []):
            if r["expected_pass"]:
                continue
            precise = (r["guard_rejected"] and not r["wait_entry"] if arm_name == "cut_wait" else
                       r["producer_gap"] if arm_name == "cut_publish" else
                       r["entry_missing"] if arm_name == "cut_entry" else r["copy_gap"])
            if not precise:
                failed.append(r)
    for before, after in (("accept", "restored"), ("reject", "reject_restored")):
        if before in manifest["arms"] and after in manifest["arms"]:
            if manifest["arms"][before]["sha256"] != manifest["arms"][after]["sha256"]:
                failed.append({"artifact_restore_mismatch": [before, after]})
    manifest["validation_failures"] = failed
    report.write_text(json.dumps(manifest, indent=2))
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
