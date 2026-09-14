#!/usr/bin/env python3
"""Keep the managed allocation fixture alive until its product cycle completes."""

import os
from pathlib import Path
import selectors
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "perf_vs_official"))
from gclog_schema import parse_gclog


def run(binary, log_path):
    deadline = time.monotonic() + 60
    acknowledged = False
    pending = b""
    with open(log_path, "wb") as log, subprocess.Popen(
        [binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT
    ) as process, selectors.DefaultSelector() as selector:
        selector.register(process.stdout, selectors.EVENT_READ)
        try:
            while selector.get_map():
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise subprocess.TimeoutExpired(binary, 60)
                for key, _ in selector.select(remaining):
                    chunk = os.read(key.fileobj.fileno(), 65536)
                    if not chunk:
                        selector.unregister(key.fileobj)
                        continue
                    log.write(chunk)
                    log.flush()
                    pending += chunk
                    while b"\n" in pending:
                        line, pending = pending.split(b"\n", 1)
                        if acknowledged or not line.startswith(b"[GCLOG] ") or b" rec=cycle " not in line:
                            continue
                        records = parse_gclog(line.decode("utf-8"))
                        for cycle in records.cycles:
                            if cycle.kind == "minor" and cycle.seq > 0:
                                process.stdin.write(b"\n")
                                process.stdin.flush()
                                acknowledged = True
                                log.write(f"PHASE_ENTRY_CYCLE_ACK seq={cycle.seq}\n".encode())
                                log.flush()
            rc = process.wait(timeout=max(0, deadline - time.monotonic()))
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            log.write(b"PHASE_ENTRY_WAIT_FAIL timeout=60\n")
            return 124
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
        if not acknowledged:
            log.write(b"PHASE_ENTRY_WAIT_FAIL completed_minor=missing\n")
            return rc or 1
        return rc if rc >= 0 else 128 - rc


if __name__ == "__main__":
    raise SystemExit(run(sys.argv[1], sys.argv[2]))
