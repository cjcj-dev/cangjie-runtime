#!/usr/bin/env python3
"""Check synchronization carrier routing in actual compiler-produced core IR."""
import re
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text()
failures = []


def check(name, result):
    print(f"REFERENCE1356_SYNC_IR_TARGET name={name} pass={bool(result)}")
    if not result:
        failures.append(name)


for carrier, layout in [("ReferenceMutex", "Mutex"), ("ReferenceMonitor", "Monitor")]:
    type_lines = [line for line in text.splitlines()
                  if line.startswith(f'@"std.core:{carrier}.ti" = ')]
    attribute = re.search(r" #(\d+)$", type_lines[0]) if len(type_lines) == 1 else None
    attributes = re.search(r"^attributes #" + attribute[1] + r" = (.*)$", text, re.M) if attribute else None
    check("typeinfo_" + carrier, attributes is not None and f'"{layout}"' in attributes[1])
    allocations = [line for line in text.splitlines()
                   if "@llvm.cj.malloc.object(" in line and f'@"std.core:{carrier}.ti"' in line]
    metadata = [re.search(r"!MallocType !(\d+)", line) for line in allocations]
    check("allocation_" + carrier, bool(allocations) and all(
        entry is not None and re.search(r"^!" + entry[1] + r' = !\{!"' + layout + r'"\}$', text, re.M)
        for entry in metadata))

check("control_reference_fields", '@"std.core:Reference.ti.offsets"' in text)
check("control_reference_publication", "call void @CJ_MCC_SetReferenceMethods(" in text)
print(f"REFERENCE1356_SYNC_IR_RESULT failures={len(failures)}")
sys.exit(bool(failures))
