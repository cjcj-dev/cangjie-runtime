#!/usr/bin/env python3
# Compatibility entry; implementation is zx ESM (cjcj#883).
import os, sys
from pathlib import Path
os.execvp('node', ['node', str(Path(__file__).with_suffix('.mjs')), *sys.argv[1:]])
