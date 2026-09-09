#!/usr/bin/env python3
"""Scope-aware source gate for PostResolveCycleTask's cycle-work snapshot.

The product body is only compiled for OHOS. This gate makes a narrower claim:
each empty() read is covered by an RAII lock that resolves to the inherited
member owner, and scheduling happens after that lock's lexical lifetime.
"""

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple


IDENT = r"[A-Za-z_]\w*"


class AnalysisError(RuntimeError):
    pass


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


def brace_pairs(source: str) -> Dict[int, int]:
    stack: List[int] = []
    pairs: Dict[int, int] = {}
    for index, char in enumerate(source):
        if char == "{":
            stack.append(index)
        elif char == "}":
            if not stack:
                raise AnalysisError("unbalanced closing brace")
            pairs[stack.pop()] = index
    if stack:
        raise AnalysisError("unbalanced opening brace")
    return pairs


def enclosing_scopes(pairs: Dict[int, int], position: int) -> List[Tuple[int, int]]:
    return sorted(
        ((opening, closing) for opening, closing in pairs.items() if opening < position < closing),
        key=lambda scope: scope[0],
    )


def function_body(source: str) -> str:
    signature = re.search(r"void\s+WCollector\s*::\s*PostResolveCycleTask\s*\(\s*\)", source)
    if signature is None:
        raise AnalysisError("product function missing")
    opening = source.find("{", signature.end())
    if opening < 0:
        raise AnalysisError("product function body missing")
    pairs = brace_pairs(source)
    if opening not in pairs:
        raise AnalysisError("product function body is unbalanced")
    return source[opening + 1:pairs[opening]]


def named_class_body(source: str, class_name: str) -> str:
    declaration = re.search(rf"class\s+{re.escape(class_name)}\b[^;{{]*{{", source)
    if declaration is None:
        raise AnalysisError(f"class {class_name} declaration missing")
    opening = source.find("{", declaration.start())
    pairs = brace_pairs(source)
    if opening not in pairs:
        raise AnalysisError(f"class {class_name} declaration is unbalanced")
    return source[opening + 1:pairs[opening]]


def verify_member_owner(owner_source: str, wcollector_source: str, copy_collector_source: str) -> None:
    owner_body = named_class_body(owner_source, "TracingCollector")
    declarations = re.findall(r"std\s*::\s*mutex\s+cycleWorkStackMtx\s*;", owner_body)
    if len(declarations) != 1:
        raise AnalysisError(
            f"expected one TracingCollector member owner declaration, found {len(declarations)}"
        )
    if re.search(r"class\s+WCollector\s*:\s*public\s+CopyCollector\b", wcollector_source) is None:
        raise AnalysisError("WCollector to CopyCollector inheritance edge missing")
    if re.search(r"class\s+CopyCollector\s*:\s*public\s+TracingCollector\b", copy_collector_source) is None:
        raise AnalysisError("CopyCollector to TracingCollector inheritance edge missing")


@dataclass(frozen=True)
class LockLifetime:
    construction: int
    release: int
    name: str
    kind: str


LOCK_DECLARATION = re.compile(
    rf"std\s*::\s*(?P<kind>lock_guard|unique_lock|scoped_lock)"
    rf"\s*(?:<[^;{{}}]+>)?\s+(?P<name>{IDENT})\s*"
    rf"(?P<open>[({{])\s*(?P<qualified>this\s*->\s*)?"
    rf"cycleWorkStackMtx\s*(?P<close>[)}}])\s*;"
)

LOCAL_OWNER_DECLARATION = re.compile(
    r"(?:std\s*::\s*)?(?:mutex|recursive_mutex|shared_mutex|shared_timed_mutex)"
    r"\s+cycleWorkStackMtx\b"
)


def is_shadowed(body: str, pairs: Dict[int, int], lock_position: int) -> bool:
    active_scopes = enclosing_scopes(pairs, lock_position)
    for declaration in LOCAL_OWNER_DECLARATION.finditer(body, 0, lock_position):
        declaration_scopes = enclosing_scopes(pairs, declaration.start())
        if declaration_scopes and declaration_scopes[-1] in active_scopes:
            return True
    return False


def lock_lifetimes(body: str, pairs: Dict[int, int]) -> List[LockLifetime]:
    locks: List[LockLifetime] = []
    for match in LOCK_DECLARATION.finditer(body):
        if (match.group("open"), match.group("close")) not in (("(", ")"), ("{", "}")):
            continue
        if match.group("qualified") is None and is_shadowed(body, pairs, match.start()):
            raise AnalysisError("lock owner resolves to a shadowing local mutex")
        scopes = enclosing_scopes(pairs, match.start())
        if not scopes:
            raise AnalysisError("RAII owner lock has no lexical lifetime")
        locks.append(LockLifetime(match.end(), scopes[-1][1], match.group("name"), match.group("kind")))
    return locks


def snapshot_name(body: str, empty_position: int) -> Optional[str]:
    statement_start = max(body.rfind(";", 0, empty_position), body.rfind("{", 0, empty_position)) + 1
    statement_end = body.find(";", empty_position)
    if statement_end < 0:
        return None
    statement = body[statement_start:statement_end]
    assignment = re.fullmatch(
        rf"\s*(?:(?:const\s+)?bool\s+)?(?P<name>{IDENT})\s*=\s*!\s*"
        rf"cycleRefWorkStack\s*\.\s*empty\s*\(\s*\)\s*",
        statement,
    )
    return assignment.group("name") if assignment else None


def scheduler_is_guarded(body: str, release: int, schedule: int, snapshot: str) -> bool:
    between = body[release:schedule]
    if re.search(
        rf"if\s*\(\s*!\s*{re.escape(snapshot)}\s*\)\s*{{?\s*return\s*;",
        between,
    ):
        return True
    pairs = brace_pairs(body)
    for guard in re.finditer(rf"if\s*\(\s*{re.escape(snapshot)}\s*\)", body[release:schedule]):
        guard_end = release + guard.end()
        opening = body.find("{", guard_end, schedule)
        if opening >= 0 and opening in pairs and opening < schedule < pairs[opening]:
            return True
        statement_end = body.find(";", guard_end)
        if statement_end >= schedule:
            return True
    return False


def analyze(
    source_text: str,
    owner_text: str,
    wcollector_text: str,
    copy_collector_text: str,
) -> Dict[str, int]:
    source = mask_comments_and_literals(source_text)
    owner_source = mask_comments_and_literals(owner_text)
    verify_member_owner(
        owner_source,
        mask_comments_and_literals(wcollector_text),
        mask_comments_and_literals(copy_collector_text),
    )
    body = function_body(source)
    pairs = brace_pairs(body)
    locks = lock_lifetimes(body, pairs)
    if not locks:
        raise AnalysisError("member cycleWorkStackMtx RAII lock missing")

    empties = list(re.finditer(r"cycleRefWorkStack\s*\.\s*empty\s*\(\s*\)", body))
    if not empties:
        raise AnalysisError("cycleRefWorkStack.empty read missing")
    covering: List[Tuple[LockLifetime, str]] = []
    for empty in empties:
        owners = [lock for lock in locks if lock.construction <= empty.start() < lock.release]
        if len(owners) != 1:
            raise AnalysisError("empty read is not covered by exactly one member owner lock")
        lock = owners[0]
        if re.search(
            rf"\b{re.escape(lock.name)}\s*\.\s*unlock\s*\(\s*\)",
            body[lock.construction:empty.start()],
        ):
            raise AnalysisError("member owner was unlocked before empty read")
        snapshot = snapshot_name(body, empty.start())
        if snapshot is None:
            raise AnalysisError("locked empty result is not captured in a boolean snapshot")
        covering.append((lock, snapshot))

    schedules = list(re.finditer(r"CJ_MRT_RolveCycleRef\s*\(\s*\)\s*;", body))
    if len(schedules) != 1:
        raise AnalysisError(f"expected one scheduler call, found {len(schedules)}")
    schedule = schedules[0].start()
    for lock, snapshot in covering:
        if schedule <= lock.release:
            raise AnalysisError("scheduler call occurs before releasing member owner lock")
        if not scheduler_is_guarded(body, lock.release, schedule, snapshot):
            raise AnalysisError("scheduler call is not controlled by the locked snapshot")
    return {
        "empty_reads": len(empties),
        "scheduler_calls": len(schedules),
        "member_owner_locks": len({(lock.construction, lock.release) for lock, _ in covering}),
    }


def fail(message: str) -> None:
    print(f"POST_RESOLVE_CYCLE_LOCK_FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--owner-declaration", required=True, type=Path)
    parser.add_argument("--wcollector-declaration", required=True, type=Path)
    parser.add_argument("--copy-collector-declaration", required=True, type=Path)
    args = parser.parse_args()
    try:
        result = analyze(
            args.source.read_text(encoding="utf-8"),
            args.owner_declaration.read_text(encoding="utf-8"),
            args.wcollector_declaration.read_text(encoding="utf-8"),
            args.copy_collector_declaration.read_text(encoding="utf-8"),
        )
    except (AnalysisError, OSError) as error:
        fail(str(error))
    print(
        "POST_RESOLVE_CYCLE_LOCK_OK "
        f"source={args.source} empty_reads={result['empty_reads']} "
        f"scheduler_calls={result['scheduler_calls']} "
        f"member_owner_locks={result['member_owner_locks']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
