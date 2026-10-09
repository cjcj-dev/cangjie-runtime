#!/usr/bin/env python3
"""Compatibility entry; orchestration lives in zx ESM."""
import os
from pathlib import Path
import sys

if __name__ == '__main__':
    os.execvp('zx', ['zx', str(Path(__file__).with_suffix('.mjs')), *sys.argv[1:]])
