#!/usr/bin/env python3
"""Known-good and known-bad arms for the PostResolveCycleTask source gate."""

import argparse
import importlib.util
import sys
import unittest
from pathlib import Path


def load_gate(path: Path):
    spec = importlib.util.spec_from_file_location("post_resolve_cycle_lock_gate", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load gate: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class GateArms(unittest.TestCase):
    def assert_rejected(self, source: str, reason: str) -> None:
        with self.assertRaisesRegex(self.gate.AnalysisError, reason) as rejected:
            self.gate.analyze(source, self.owner_text, self.wcollector_text, self.copy_collector_text)
        print(f"GATE_ARM_REJECT expected={reason} actual={rejected.exception}")

    def test_candidate_is_accepted(self) -> None:
        result = self.gate.analyze(
            self.source_text, self.owner_text, self.wcollector_text, self.copy_collector_text
        )
        self.assertEqual(result["empty_reads"], 1)
        print("GATE_ARM_ACCEPT arm=candidate")

    def test_shadowing_local_owner_is_rejected(self) -> None:
        source = self.source_text.replace(
            "std::lock_guard<std::mutex> lock(cycleWorkStackMtx);",
            "std::mutex cycleWorkStackMtx;\n        std::lock_guard<std::mutex> lock(cycleWorkStackMtx);",
            1,
        )
        self.assert_rejected(source, "shadowing local mutex")

    def test_unique_lock_is_accepted(self) -> None:
        source = self.source_text.replace(
            "std::lock_guard<std::mutex> lock(cycleWorkStackMtx);",
            "std::unique_lock<std::mutex> lock(cycleWorkStackMtx);",
            1,
        )
        result = self.gate.analyze(
            source, self.owner_text, self.wcollector_text, self.copy_collector_text
        )
        self.assertEqual(result["member_owner_locks"], 1)
        print("GATE_ARM_ACCEPT arm=unique_lock")

    def test_missing_owner_lock_is_rejected(self) -> None:
        source = self.source_text.replace(
            "std::lock_guard<std::mutex> lock(cycleWorkStackMtx);", "", 1
        )
        self.assert_rejected(source, "RAII lock missing")

    def test_scheduler_inside_owner_lifetime_is_rejected(self) -> None:
        source = self.source_text.replace(
            "        shouldPost = !cycleRefWorkStack.empty();\n"
            "    }\n"
            "    if (!shouldPost) {\n"
            "        return;\n"
            "    }\n"
            "    CJ_MRT_RolveCycleRef();\n",
            "        shouldPost = !cycleRefWorkStack.empty();\n"
            "        CJ_MRT_RolveCycleRef();\n"
            "    }\n"
            "    if (!shouldPost) {\n"
            "        return;\n"
            "    }\n",
            1,
        )
        self.assert_rejected(source, "before releasing member owner lock")

    def test_snapshot_identifier_is_not_part_of_contract(self) -> None:
        source = self.source_text.replace("shouldPost", "pendingCycleWork")
        result = self.gate.analyze(
            source, self.owner_text, self.wcollector_text, self.copy_collector_text
        )
        self.assertEqual(result["scheduler_calls"], 1)
        print("GATE_ARM_ACCEPT arm=renamed_snapshot")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gate", required=True, type=Path)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--owner-declaration", required=True, type=Path)
    parser.add_argument("--wcollector-declaration", required=True, type=Path)
    parser.add_argument("--copy-collector-declaration", required=True, type=Path)
    args, _ = parser.parse_known_args()
    GateArms.gate = load_gate(args.gate)
    GateArms.source_text = args.source.read_text(encoding="utf-8")
    GateArms.owner_text = args.owner_declaration.read_text(encoding="utf-8")
    GateArms.wcollector_text = args.wcollector_declaration.read_text(encoding="utf-8")
    GateArms.copy_collector_text = args.copy_collector_declaration.read_text(encoding="utf-8")
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(GateArms)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
