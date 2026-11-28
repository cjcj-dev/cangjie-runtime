#!/usr/bin/env python3
# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
"""Build and exercise the queue and plain-path backports with a Linux x64 SDK."""

import argparse
from collections import Counter
import difflib
import hashlib
import os
from pathlib import Path
import re
import resource
import select
import shlex
import shutil
import subprocess
import time


ROOT = Path(__file__).resolve().parents[3]
TEST = Path(__file__).resolve().parent
STDLIB = ROOT / "stdlib/libs/std"
BASE = "cebfdebd7528069b2d3c9b56cc46c08ba6c67652"
TRIPLE = "linux_x86_64_cjnative"
QUEUE = "non_blocking_queue.cj"
PACKAGES = {
    "collection.concurrent": "collection/concurrent",
    "convert": "convert",
    "fs": "fs",
}
CONVERT = ("base.cj", "parsable.cj")
A2_MARKERS = {
    "base.cj": ("func getBaseInfos(typeBase: TypeBase, isNegative: Int64, rawData: Array<Byte> @local?)",
                "local getBaseInfos"),
    "parsable.cj": ("func parseCompInt(rawData: Array<Byte> @local?,", "local parseCompInt"),
    "file.cj": ("private func checkCanRead(this @local?)", "local checkCanRead"),
}
BLOCKING_RED = "FAIL: blocking successor did not recover after failed growth"
LINKED_RED = "FAIL: linked contender did not reclaim the failed extension"


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def git(*args):
    return subprocess.check_output(["git", "-C", ROOT, *args], text=True)


def replace_once(source, old, new):
    require(source.count(old) == 1, f"expected one source anchor: {old!r}")
    return source.replace(old, new)


def insert_marker(source, declaration, marker):
    require(source.count(declaration) == 1, f"expected one local overload: {declaration}")
    start = source.index(declaration)
    body = source.index("{", start) + 1
    return source[:body] + f'\n        println("ROUTE: {marker}")' + source[body:]


def prepare(pkg, arm, destination, logs):
    src = STDLIB / PACKAGES[pkg]
    destination.mkdir(parents=True)
    patches = []
    for source in sorted(src.glob("*.cj")):
        name = source.name
        original = source.read_text()
        result = original
        from_base = (
            (pkg == "collection.concurrent" and name == QUEUE and arm in {"base", "cut-a1"})
            or (pkg == "convert" and name in CONVERT and arm in {"base", "cut-a2", "cut-parse"})
            or (pkg == "fs" and name == "file.cj" and arm in {"base", "cut-a2", "cut-read"})
        )
        if from_base:
            result = git("show", f"{BASE}:{source.relative_to(ROOT)}")
        if pkg == "collection.concurrent" and name == QUEUE and arm in {
            "fault", "cut-a1", "cut-reset", "cut-reclaim"
        }:
            if arm == "cut-reset":
                result = replace_once(result, "        tailBlock.isExtended.store(false)\n", "")
            elif arm == "cut-reclaim":
                result = replace_once(
                    result,
                    "            if (tailBlock.isExtended.compareAndSwap(false, true)) {\n"
                    "                break\n            }\n", ""
                )
            result = replace_once(
                result, "    if (!tailBlock.isExtended.compareAndSwap(false, true)) {\n",
                "    if (!tailBlock.isExtended.compareAndSwap(false, true)) {\n"
                "        observeQueueGrowthWaiter()\n",
            )
            result = replace_once(
                result, "let newBlock = Block<E>(tailBlock.version + 1)",
                "failQueueGrowthAllocation()\n" + ("    " if arm == "cut-a1" else "        ")
                + "let newBlock = Block<E>(tailBlock.version + 1)",
            )
        if arm in {"route", "cut-a2", "cut-parse", "cut-read"} and (
            (pkg == "convert" and name in CONVERT) or (pkg == "fs" and name == "file.cj")
        ):
            result = insert_marker(result, *A2_MARKERS[name])
            if name == "file.cj":
                result = insert_marker(
                    result, "private func directRead(this @local?,", "local directRead"
                )
        (destination / name).write_text(result)
        patches.extend(difflib.unified_diff(
            original.splitlines(keepends=True), result.splitlines(keepends=True),
            fromfile=f"candidate/{name}", tofile=f"{arm}/{name}",
        ))
    if pkg == "collection.concurrent" and arm in {"fault", "cut-a1", "cut-reset", "cut-reclaim"}:
        shutil.copyfile(TEST / "queue_fault.cj", destination / "queue_fault.cj")
    (logs / f"{arm}.{pkg}.diff").write_text("".join(patches))


class Runner:
    def __init__(self, sdk, out, modes, group):
        self.sdk = sdk
        self.out = out
        self.modes = modes
        self.group = group
        self.out.mkdir()
        self.logs = self.out / "evidence"
        self.logs.mkdir()
        self.cjc = self.sdk / "bin/cjc"
        self.runtime = self.sdk / "runtime/lib" / TRIPLE
        self.native = self.sdk / "lib" / TRIPLE
        self.env = os.environ.copy()
        self.env["CANGJIE_HOME"] = str(sdk)
        self.env["PATH"] = f"{sdk / 'bin'}:{self.env['PATH']}"
        self.env["LD_LIBRARY_PATH"] = ":".join(map(str, [
            self.runtime, self.native, sdk / "third_party/llvm/lib",
        ]))
        self.env["LC_ALL"] = "C"
        self.warnings = {}

    def invoke(self, tag, cmd, *, env=None, timeout=180, expected_rc=0, contains=()):
        cmd = list(map(str, cmd))
        with (self.logs / "commands.log").open("a") as stream:
            print(f"{tag}: {shlex.join(cmd)}", file=stream)
        result = subprocess.run(
            cmd, cwd=self.out, env=self.env if env is None else env,
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=timeout, check=False, errors="replace",
        )
        (self.logs / f"{tag}.log").write_text(result.stdout)
        with (self.logs / "rc.tsv").open("a") as stream:
            print(tag, result.returncode, sep="\t", file=stream)
        require(result.returncode == expected_rc, f"{tag} rc={result.returncode}: {result.stdout[-2000:]}")
        for item in contains:
            require(item in result.stdout, f"{tag}: expected {item!r}")
        return result.stdout

    def sha(self, path):
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        with (self.logs / "sha256.txt").open("a") as stream:
            print(digest.hexdigest(), path, file=stream)

    def mode_flags(self, mode):
        return ["--experimental", "--enable-local-mode", "--cfg", f"local_mode_api={mode}"]

    def overlay(self, mode, arm):
        return self.out / mode / arm

    def package(self, mode, arm, pkg, fallback=None):
        overlay = self.overlay(mode, arm)
        mods = overlay / "std"
        mods.mkdir(parents=True, exist_ok=True)
        src = overlay / "src" / pkg
        prepare(pkg, arm, src, self.logs)
        so = overlay / f"libcangjie-std-{pkg}.so"
        ffi = ["-l", f":libcangjie-std-{pkg}FFI.a"]
        cmd = [
            self.cjc, *self.mode_flags(mode), "--module-name", "std", "--no-sub-pkg",
            "-g", "--apc=1", "-j1", "-O2", "--output-type=dylib", "--dy-std",
            "--import-path", overlay, "-L", overlay,
        ]
        if fallback is not None:
            cmd += ["--import-path", fallback, "-L", fallback]
        cmd += ["-L", self.native, *ffi,
                "-l", "boundscheck", "-p", src, "-o", so]
        log = self.invoke(f"{mode}.{arm}.build.{pkg}", cmd)
        plain_log = re.sub(r"\x1b\[[0-9;]*m", "", log)
        self.warnings[mode, arm, pkg] = Counter(re.findall(r"(?m)^warning:.*$", plain_log))
        require(so.is_file(), f"missing linked product {so}")
        cjo = overlay / f"std.{pkg}.cjo"
        if cjo.is_file():
            cjo.rename(mods / cjo.name)
        require((mods / cjo.name).is_file(), f"missing exported declarations {cjo}")
        self.sha(so)

    def binary(self, mode, name, source, preferred, fallback):
        exe = self.overlay(mode, preferred) / name
        p = self.overlay(mode, preferred)
        f = self.overlay(mode, fallback)
        self.invoke(f"{mode}.{preferred}.link.{name}", [
            self.cjc, *self.mode_flags(mode), "-g", "--dy-std",
            "--import-path", p, "--import-path", f, "-L", p, "-L", f,
            TEST / source, "-o", exe,
        ])
        self.sha(exe)
        return exe

    def runtime_environment(self, mode, arm):
        preferred = self.overlay(mode, arm)
        fixed = self.overlay(mode, "fixed")
        env = dict(self.env)
        env["LD_LIBRARY_PATH"] = f"{preferred}:{fixed}:{env['LD_LIBRARY_PATH']}"
        env["cjHeapSize"] = "128MB"
        env["cjProcessorNum"] = "4"
        return env

    def app(self, mode, arm, exe, test, target, *, expected_rc=0, contains=(), case=None):
        preferred = self.overlay(mode, arm)
        fixed = self.overlay(mode, "fixed")
        env = self.runtime_environment(mode, arm)
        tag = f"{mode}.{arm}.run.{test}"
        so = f"libcangjie-std-{target}.so"
        expected_so = preferred / so if (preferred / so).exists() else fixed / so
        loaded = self.invoke(f"{tag}.ldd", ["ldd", exe], env=env)
        require(str(expected_so) in loaded, f"{tag}: expected loaded package {expected_so}")
        for leaf in ("libcangjie-runtime.so", "libboundscheck.so"):
            require(str(self.runtime / leaf) in loaded, f"{tag}: unexpected {leaf} lineage")
        require("not found" not in loaded, f"{tag}: loader could not find a dependency")
        return self.invoke(
            tag, ["timeout", "15", exe, case or test, self.out / "temporary-file"], env=env, timeout=20,
            expected_rc=expected_rc, contains=contains,
        )

    def blocked_read(self, mode, exe):
        tag = f"{mode}.fixed.run.blocked"
        fifo = self.out / f"{mode}.fifo"
        os.mkfifo(fifo)
        command = [str(exe), "blocked", str(fifo)]
        with (self.logs / "commands.log").open("a") as stream:
            print(f"{tag}: {shlex.join(command)}", file=stream)
        result = bytearray()
        env = self.runtime_environment(mode, "fixed")
        # The SDK's generational collector updates getGCCount() after collection.
        env["cjEnableGC"] = "2"
        try:
            with subprocess.Popen(command, cwd=self.out, env=env,
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT) as child:
                try:
                    proc = Path(f"/proc/{child.pid}")
                    until = time.monotonic() + 5
                    blocked = None
                    while blocked is None and child.poll() is None and time.monotonic() < until:
                        fds = set()
                        for fd in (proc / "fd").glob("*"):
                            try:
                                if os.readlink(fd) == str(fifo):
                                    fds.add(int(fd.name))
                            except FileNotFoundError:
                                continue
                        for syscall in (proc / "task").glob("*/syscall"):
                            try:
                                fields = syscall.read_text().split()
                            except (FileNotFoundError, ProcessLookupError):
                                continue
                            # Linux x86-64: syscall 0 is read; its first argument is the fd.
                            if len(fields) > 1 and fields[0] == "0" and int(fields[1], 0) in fds:
                                blocked = f"{syscall.parent.name}: {' '.join(fields)}"
                                break
                        if blocked is None:
                            time.sleep(0.01)
                    require(blocked is not None, "did not observe File.read on the empty FIFO")
                    result.extend(f"native read: {blocked}\n".encode())
                    child.stdin.write(b"collect\n")
                    child.stdin.flush()
                    until = time.monotonic() + 5
                    while b"COLLECTION: " not in result and time.monotonic() < until:
                        if not select.select([child.stdout], [], [], max(0, until - time.monotonic()))[0]:
                            break
                        data = os.read(child.stdout.fileno(), 4096)
                        if not data:
                            break
                        result.extend(data)
                    require(b"COLLECTION: " in result, "no completed collection while FIFO was empty")
                    fd = os.open(fifo, os.O_WRONLY | os.O_NONBLOCK)
                    try:
                        require(os.write(fd, b"*") == 1, "could not release the FIFO read")
                    finally:
                        os.close(fd)
                    result.extend(child.communicate(timeout=5)[0])
                    require(child.returncode == 0 and b"PASS: collection completed" in result,
                            "read after collection failed")
                finally:
                    if child.poll() is None:
                        child.kill()
                        result.extend(child.communicate()[0])
                    with (self.logs / "rc.tsv").open("a") as stream:
                        print(tag, child.returncode, sep="\t", file=stream)
        finally:
            fifo.unlink()
            (self.logs / f"{tag}.log").write_bytes(result)

    def routes(self, mode, arm, exe, red=(), restored=False):
        for test, package, expected in (
            ("convert", "convert", {"local getBaseInfos", "local parseCompInt"}),
            ("fs", "fs", {"local checkCanRead", "local directRead"}),
        ):
            label = test + "-restored" if restored else test
            out = self.app(mode, arm, exe, label, package, contains=("PASS: plain",), case=test)
            observed = set(re.findall(r"(?m)^ROUTE: (.*)$", out))
            want = expected if test in red else set()
            tag = f"{mode}.{arm}.dispatch.{label}"
            verdict = "PASS" if not observed else "FAIL"
            with (self.logs / "dispatch.tsv").open("a") as stream:
                print(tag, verdict, ", ".join(sorted(observed)), sep="\t", file=stream)
            require(observed == want, f"{tag}: observed {observed!r}, expected {want!r}")

    def run(self):
        try:
            self.invoke("uptime-before", ["uptime"])
            self.invoke("cores", ["taskset", "-pc", os.getpid()])
            self.invoke("compiler", [self.cjc, "-v"], contains=("1.3.0-alpha.20261124001016",))
            self.sha(self.cjc)
            for leaf in ("libcangjie-runtime.so", "libboundscheck.so"):
                self.sha(self.runtime / leaf)
            for pkg in ("collection.concurrent", "convert", "fs"):
                self.sha(self.native / f"libcangjie-std-{pkg}FFI.a")
            (self.logs / "source.txt").write_text(git("rev-parse", "HEAD"))
            for mode in self.modes:
                self.run_mode(mode)
            require(not git("status", "--porcelain"), "source worktree changed")
        finally:
            self.invoke("uptime-after", ["uptime"])

    def run_mode(self, mode):
        fixed = self.overlay(mode, "fixed")
        packages = {
            "a1": ("collection.concurrent",),
            "a2": ("convert", "fs"),
            "both": ("collection.concurrent", "convert", "fs"),
        }[self.group]
        for pkg in packages:
            self.package(mode, "fixed", pkg)
            self.package(mode, "base", pkg, fixed)
            extra = self.warnings[mode, "fixed", pkg] - self.warnings[mode, "base", pkg]
            with (self.logs / "warnings.tsv").open("a") as stream:
                print(mode, pkg, sum(self.warnings[mode, "base", pkg].values()),
                      sum(self.warnings[mode, "fixed", pkg].values()), sum(extra.values()),
                      sep="\t", file=stream)
            require(not extra, f"{mode} {pkg}: new build warnings: {extra}")
        plain = None
        queue = None
        if self.group != "a1":
            plain = self.binary(mode, "plain", "plain_paths.cj", "fixed", "fixed")
            self.routes(mode, "fixed", plain)
            self.blocked_read(mode, plain)
        if self.group != "a2":
            self.package(mode, "fault", "collection.concurrent", fixed)
            queue = self.binary(mode, "queue", "queue_main.cj", "fault", "fixed")
            self.queues(mode, plain, queue)
        if plain is not None:
            self.plain_paths(mode, plain, queue)

    def queues(self, mode, plain, queue):
        fixed = self.overlay(mode, "fixed")
        for test in ("blocking", "linked"):
            self.app(mode, "fault", queue, test, "collection.concurrent", contains=("PASS: ",))

        for arm, reds in (
            ("cut-a1", {"blocking": BLOCKING_RED, "linked": LINKED_RED}),
            ("cut-reset", {"blocking": BLOCKING_RED, "linked": LINKED_RED}),
            ("cut-reclaim", {"linked": LINKED_RED}),
        ):
            self.package(mode, arm, "collection.concurrent", fixed)
            for test in ("blocking", "linked"):
                self.app(
                    mode, arm, queue, test, "collection.concurrent",
                    expected_rc=1 if test in reds else 0,
                    contains=(reds.get(test, "PASS: "), "before allocation failed" if test == "linked"
                              else "full-block count intact"),
                )
            if plain is not None:
                self.routes(mode, arm, plain)
        for test in ("blocking", "linked"):
            self.app(mode, "fault", queue, test + "-restored", "collection.concurrent",
                     contains=("PASS: ",), case=test)

    def plain_paths(self, mode, plain, queue):
        fixed = self.overlay(mode, "fixed")
        for arm, changed in (
            ("route", {"convert", "fs"}),
            ("cut-a2", {"convert", "fs"}),
            ("cut-parse", {"convert"}),
            ("cut-read", {"fs"}),
        ):
            for pkg in sorted(changed):
                self.package(mode, arm, pkg, fixed)
            self.routes(mode, arm, plain, red=changed if mode == "on" and arm != "route" else ())
            if mode == "on" and arm == "route":
                explicit = self.binary(mode, "local-read", "local_read.cj", "fixed", "fixed")
                self.app(mode, arm, explicit, "local", "fs", contains=(
                    "PASS: explicit local File.read", "ROUTE: local checkCanRead", "ROUTE: local directRead",
                ))
            if arm != "route" and queue is not None:
                # Use the fixed fault-injection queue with each A2 cut as
                # fallback so all controls run on the same library overlay.
                shutil.copyfile(
                    self.overlay(mode, "fault") / "libcangjie-std-collection.concurrent.so",
                    self.overlay(mode, arm) / "libcangjie-std-collection.concurrent.so",
                )
                for test in ("blocking", "linked"):
                    self.app(mode, arm, queue, test, "collection.concurrent", contains=("PASS: ",))
        self.routes(mode, "route", plain, restored=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True, help="new disposable output directory")
    parser.add_argument("--mode", choices=("off", "on"), action="append")
    parser.add_argument("--group", choices=("a1", "a2", "both"), default="both")
    options = parser.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    require(not git("status", "--porcelain"), "expected a clean runtime worktree")
    Runner(options.sdk.resolve(), options.out.resolve(), options.mode or ("off", "on"),
           options.group).run()


if __name__ == "__main__":
    main()
