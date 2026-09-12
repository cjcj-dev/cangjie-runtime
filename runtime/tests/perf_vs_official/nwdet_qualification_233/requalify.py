#!/usr/bin/env python3
"""Check retained NW link identities before invoking the qualification gate."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys


# #278 traced the OOME reference store to this official core.o. A different
# digest is not proof of colouring: the producer receipt still needs review.
FORBIDDEN_STD_SHA256 = {
    "9619dc3e2b3ab335a340d9d7b2e6e86c8e0103704a152b953a20a56bf5eab9cb",
}
NOT_RUN_EXIT = 3


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def qualification_preflight(manifest, binary):
    """Bind the link record to this ELF; never infer colour from a filename."""
    try:
        value = json.loads(manifest.read_text())
        linkage = value["linkage"]
        if linkage["elf_sha256"] != sha256(binary):
            return False, "linkage-elf-sha256-mismatch"
        archives = linkage["static_std_archives"]
        if not isinstance(archives, list) or not archives:
            return False, "static-std-linkage-missing"
        for archive in archives:
            if not isinstance(archive["path"], str) or not archive["path"]:
                return False, "static-std-linkage-path"
            digest = archive["sha256"]
            if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", digest):
                return False, "static-std-linkage-sha256"
            if digest.lower() in FORBIDDEN_STD_SHA256:
                return False, "负载链接了无色官方 std"
        if linkage["std_origin"] != "stage1-coloured":
            return False, "stage1-coloured-std-required"
        # A receipt is mandatory even when the known bad digest is absent.
        # Its qualification is the producer/reviewer's responsibility (#32).
        receipt = linkage["std_build_receipt"]
        receipt_path = Path(receipt["path"])
        if not receipt_path.is_absolute():
            return False, "std-build-receipt-path-must-be-absolute"
        if sha256(receipt_path) != receipt["sha256"]:
            return False, "std-build-receipt-sha256-mismatch"
    except (OSError, ValueError, KeyError, TypeError):
        return False, "linkage-or-std-build-receipt-unreadable"
    return True, "link-identities-checked-not-workload-qualification"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--gate", type=Path)
    parser.add_argument("--preflight-only", action="store_true")
    args = parser.parse_args()
    if not args.preflight_only and args.gate is None:
        parser.error("--gate is required unless --preflight-only is used")
    binary = args.elf.resolve()
    admitted, reason = qualification_preflight(args.manifest, binary)
    print(f"NWDET_LINK_PREFLIGHT={'RUN' if admitted else 'NOT_RUN'} reason={reason}", flush=True)
    if not admitted:
        print("NWDET: NOT_RUN(unqualified workload/admission ruler)", flush=True)
        return NOT_RUN_EXIT
    if args.preflight_only:
        return 0
    # Bind the gate's actual input, overriding an inherited BIN. The gate's
    # independent qualification file and runtime checks remain in force.
    env = dict(os.environ, BIN=str(binary))
    return subprocess.run([sys.executable, str(args.gate.resolve())], env=env).returncode


if __name__ == "__main__":
    sys.exit(main())
