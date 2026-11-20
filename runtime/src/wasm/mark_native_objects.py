"""Bind the audited C6 native ABI to the exact objects produced by this target."""
from hashlib import sha256
from pathlib import Path
from subprocess import check_call, check_output
import sys
from tempfile import TemporaryDirectory

ar, archive, *members = sys.argv[1:]
with TemporaryDirectory(dir=Path(archive).parent) as stage:
    for member in members:
        obj = check_output([ar, "p", archive, member])
        if not obj.startswith(b"\0asm\1\0\0\0"):
            raise ValueError(f"{member}: expected a WASM object")
        # Canonical last custom section; count zero denotes native code.
        name = b"cj.roots.v2"
        count = bytes(4)
        body = bytes([len(name)]) + name + count + sha256(obj + count).digest()
        (Path(stage) / member).write_bytes(obj + bytes([0, len(body)]) + body)
    check_call([ar, "rs", archive, *members], cwd=stage)
