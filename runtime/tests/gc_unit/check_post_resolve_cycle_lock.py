#!/usr/bin/env python3
"""Structural regression gate for PostResolveCycleTask's cycle-work snapshot.

The OHOS-only body cannot be executed by the native gc_unit configuration.
This gate therefore makes the lock boundary mechanically checkable without
pretending to be a runtime concurrency test.
"""

import argparse
import re
import sys
from pathlib import Path


def mask_comments_and_literals(source: str) -> str:
    """Replace comments and literals with spaces while retaining offsets."""
    result = list(source)
    index = 0
    state = "code"
    quote = ""
    while index < len(source):
        char = source[index]
        nxt = source[index + 1] if index + 1 < len(source) else ""
        if state == "code":
            if char == "/" and nxt == "/":
                result[index] = result[index + 1] = " "
                index += 2
                state = "line_comment"
                continue
            if char == "/" and nxt == "*":
                result[index] = result[index + 1] = " "
                index += 2
                state = "block_comment"
                continue
            if char in ('"', "'"):
                quote = char
                result[index] = " "
                index += 1
                state = "literal"
                continue
        elif state == "line_comment":
            if char == "\n":
                state = "code"
            else:
                result[index] = " "
            index += 1
            continue
        elif state == "block_comment":
            result[index] = " "
            if char == "*" and nxt == "/":
                result[index + 1] = " "
                index += 2
                state = "code"
                continue
            index += 1
            continue
        else:
            result[index] = " "
            if char == "\\" and nxt:
                result[index + 1] = " "
                index += 2
                continue
            if char == quote:
                state = "code"
            index += 1
            continue
        index += 1
    return "".join(result)


def matching_brace(source: str, opening: int) -> int:
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return index
    raise ValueError("unbalanced braces")


def fail(message: str) -> None:
    print(f"POST_RESOLVE_CYCLE_LOCK_FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    args = parser.parse_args()

    source = mask_comments_and_literals(args.source.read_text(encoding="utf-8"))
    signature = re.search(r"void\s+WCollector::PostResolveCycleTask\s*\(\s*\)", source)
    if signature is None:
        fail("product function missing")
    body_open = source.find("{", signature.end())
    if body_open < 0:
        fail("product function body missing")
    try:
        body_close = matching_brace(source, body_open)
    except ValueError as error:
        fail(str(error))
    body = source[body_open + 1:body_close]

    empties = list(re.finditer(r"cycleRefWorkStack\s*\.\s*empty\s*\(\s*\)", body))
    if len(empties) != 1:
        fail(f"expected one cycleRefWorkStack.empty read, found {len(empties)}")

    lock = re.search(
        r"std\s*::\s*lock_guard\s*<\s*std\s*::\s*mutex\s*>\s+\w+\s*"
        r"\(\s*cycleWorkStackMtx\s*\)\s*;",
        body,
    )
    if lock is None:
        fail("cycleWorkStackMtx lock_guard missing")

    lock_block_open = body.rfind("{", 0, lock.start())
    if lock_block_open < 0:
        fail("lock_guard has no nested release scope")
    try:
        lock_block_close = matching_brace(body, lock_block_open)
    except ValueError as error:
        fail(str(error))
    empty_pos = empties[0].start()
    if not (lock.end() <= empty_pos < lock_block_close):
        fail("empty read is outside the cycleWorkStackMtx lock scope")

    assignment = re.search(
        r"shouldPost\s*=\s*!\s*cycleRefWorkStack\s*\.\s*empty\s*\(\s*\)\s*;",
        body,
    )
    if assignment is None or not (lock.end() <= assignment.start() < lock_block_close):
        fail("shouldPost snapshot is not assigned under the owner lock")

    schedule_calls = list(re.finditer(r"CJ_MRT_RolveCycleRef\s*\(\s*\)\s*;", body))
    if len(schedule_calls) != 1:
        fail(f"expected one scheduler call, found {len(schedule_calls)}")
    schedule_pos = schedule_calls[0].start()
    if schedule_pos <= lock_block_close:
        fail("scheduler call must occur after releasing cycleWorkStackMtx")
    decision = re.search(r"if\s*\(\s*!\s*shouldPost\s*\)", body[lock_block_close:schedule_pos])
    if decision is None:
        fail("scheduler call is not guarded by the locked snapshot")

    print(
        "POST_RESOLVE_CYCLE_LOCK_OK "
        f"source={args.source} empty_reads=1 scheduler_calls=1 scheduler_outside_lock=1"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
