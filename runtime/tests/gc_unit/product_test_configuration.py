#!/usr/bin/env python3
# Compatibility entry; implementation is zx ESM.
import os, sys
from pathlib import Path
os.execvp('zx', ['zx', str(Path(__file__).with_suffix('.mjs')), *sys.argv[1:]])
