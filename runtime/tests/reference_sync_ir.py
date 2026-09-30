#!/usr/bin/env python3
"""Check synchronization carrier routing in actual compiler-produced core IR."""
import re
import sys
from pathlib import Path

source = Path(sys.argv[1])
modules = [path.read_text() for path in sorted(source.parent.glob("*-std.core.ll"))]
failures = []


def check(name, result):
    print(f"REFERENCE1356_SYNC_IR_TARGET name={name} pass={bool(result)}")
    if not result:
        failures.append(name)


for carrier, layout in [("ReferenceMutex", "Mutex"), ("ReferenceMonitor", "Monitor")]:
    type_attributes = []
    allocations = []
    for text in modules:
        for line in text.splitlines():
            if line.startswith(f'@"std.core:{carrier}.ti" = '):
                attribute = re.search(r" #(\d+)$", line)
                attributes = re.search(r"^attributes #" + attribute[1] + r" = (.*)$", text, re.M) if attribute else None
                type_attributes.append(attributes is not None and f'"{layout}"' in attributes[1])
            if "@llvm.cj.malloc.object(" in line and f'@"std.core:{carrier}.ti"' in line:
                allocations.append((text, re.search(r"!MallocType !(\d+)", line)))
    check("typeinfo_" + carrier, bool(type_attributes) and all(type_attributes))
    check("allocation_" + carrier, bool(allocations) and all(
        entry is not None and re.search(r"^!" + entry[1] + r' = !\{!"' + layout + r'"\}$', text, re.M)
        for text, entry in allocations))

check("control_reference_fields", any('@"std.core:Reference.ti.offsets"' in text for text in modules))
check("control_reference_publication", any("call void @CJ_MCC_SetReferenceMethods(" in text for text in modules))
print(f"REFERENCE1356_SYNC_IR_RESULT failures={len(failures)}")
sys.exit(bool(failures))
