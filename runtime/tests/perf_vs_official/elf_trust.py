#!/usr/bin/env python3
"""Count colour-trust fast paths vs peel-mask immediates in a CJ ELF."""
from __future__ import annotations
import hashlib, subprocess, sys
from pathlib import Path


def trust_peel(path: Path) -> tuple[str, int, int]:
    dump = subprocess.check_output(["objdump", "-d", str(path)], text=True, stderr=subprocess.DEVNULL)
    lines = dump.splitlines()
    trust = 0
    for i, line in enumerate(lines):
        if "shr" in line and "0x30" in line:
            window = lines[i : i + 4]
            if any("\tje " in w or "\tjne " in w for w in window):
                trust += 1
    peel = sum(1 for line in lines if "movabs" in line and "ffffffffffff" in line)
    sha = hashlib.sha256(path.read_bytes()).hexdigest()
    return sha, trust, peel


def main() -> None:
    print("path\tsha256\ttrust_shr30_je\tpeel_movabs")
    for raw in sys.argv[1:]:
        path = Path(raw)
        sha, trust, peel = trust_peel(path)
        print(f"{path}\t{sha}\t{trust}\t{peel}")


if __name__ == "__main__":
    main()
