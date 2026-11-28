#!/usr/bin/env python3
# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

"""Exercise the stdlib build CLI, generated compiler commands and install rules."""

from pathlib import Path
import runpy
import shlex
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


STDLIB = Path(__file__).resolve().parents[1]
DEBUG = "CANGJIE_STDLIB_DEBUG_INFO"
BITCODE = "CANGJIE_STDLIB_BUILD_LTO_BC"


def run(*argv):
    return subprocess.run(argv, check=True, text=True, stdout=subprocess.PIPE).stdout


class StdlibBuildOptions(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        work = tempfile.TemporaryDirectory()
        cls.addClassCleanup(work.cleanup)
        root = Path(work.name)
        source = root / "src"
        source.mkdir()
        (source / "sample.cj").write_text("package sample\n", encoding="utf-8")
        (source / "CMakeLists.txt").write_text(
            """cmake_minimum_required(VERSION 3.16.5)
project(StdlibBuildOptions NONE)
set(CANGJIE_CODEGEN_CJNATIVE_BACKEND ON)
set(TARGET_TRIPLE_DIRECTORY_PREFIX linux_x86_64)
include("@MODULE@")
add_cangjie_library(sample IS_PACKAGE IS_STDLIB IS_CJNATIVE_BACKEND
    MODULE_NAME std PACKAGE_NAME sample SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
""".replace("@MODULE@", (STDLIB / "cmake/modules/AddCangjieSource.cmake").as_posix()),
            encoding="utf-8",
        )
        cls.commands = {}
        cls.install = {}
        for mode, *flags in [
            ("default",),
            ("no_debug", f"-D{DEBUG}=OFF"),
            ("no_bc", f"-D{BITCODE}=OFF"),
            ("fast", f"-D{DEBUG}=OFF", f"-D{BITCODE}=OFF"),
            ("save", f"-D{BITCODE}=OFF", "-DCANGJIE_BUILD_ARGS=--save-temps=explicit"),
        ]:
            build = root / mode
            run("cmake", "-S", source, "-B", build, "-G", "Ninja",
                "-DCMAKE_BUILD_TYPE=Release", *flags)
            commands = run("ninja", "-C", build, "-t", "commands", "all")
            cls.commands[mode] = []
            for line in commands.splitlines():
                tokens = shlex.split(line)
                for i, token in enumerate(tokens):
                    if token in ("cjc", "cjc.exe"):
                        end = tokens.index("&&", i) if "&&" in tokens[i:] else len(tokens)
                        cls.commands[mode].append(tokens[i + 1:end])
            cls.install[mode] = (build / "cmake_install.cmake").read_text(encoding="utf-8")

    def test_debug_flag(self):
        for mode, expected in [("default", True), ("no_debug", False),
                               ("no_bc", True), ("fast", False)]:
            with self.subTest(mode=mode):
                self.assertTrue(self.commands[mode], "archive command missing")
                for args in self.commands[mode]:
                    self.assertEqual("-g" in args, expected, args)

    def test_bitcode_command_and_install(self):
        for mode, expected in [("default", True), ("no_debug", True),
                               ("no_bc", False), ("fast", False)]:
            with self.subTest(mode=mode):
                commands = self.commands[mode]
                lto = [args for args in commands if "--lto=full" in args]
                self.assertEqual(len(lto), int(expected), commands)
                if expected:
                    self.assertTrue(lto[0][lto[0].index("--output") + 1].endswith("libstd.sample.bc"))
                self.assertEqual("libstd.sample.bc" in self.install[mode], expected)

    def test_save_temps(self):
        for mode, expected in [("default", True), ("no_debug", True),
                               ("no_bc", False), ("fast", False), ("save", True)]:
            with self.subTest(mode=mode):
                native = [a for a in self.commands[mode] if "--lto=full" not in a]
                self.assertEqual(len(native), 1)
                saved = [arg for arg in native[0] if arg.startswith("--save-temps=")]
                self.assertEqual(len(saved), int(expected), native)
                if mode == "save":
                    self.assertEqual(saved, ["--save-temps=explicit"])

    def test_native_archive_and_module(self):
        for mode, commands in self.commands.items():
            with self.subTest(mode=mode):
                native = [args for args in commands if "--lto=full" not in args]
                self.assertEqual(len(native), 1)
                args = native[0]
                self.assertTrue(args[args.index("--output") + 1].endswith("std.sample.a"))
                self.assertIn("--output-type=staticlib", args)
                self.assertIn("--apc=1", args)
                self.assertIn("-O2", args)
                self.assertIn("std.sample.cjo", self.install[mode])

    def test_build_cli_reconfigures_both_cache_values(self):
        main = runpy.run_path(str(STDLIB / "build.py"))["main"]
        generate = main.__globals__["generate_cmake_defs"]
        for flags, expected in [
            ([], ("ON", "ON")),
            (["--stdlib-debug-info=off", "--stdlib-build-lto-bc=off"], ("OFF", "OFF")),
            (["--stdlib-debug-info=off"], ("OFF", "ON")),
            (["--stdlib-build-lto-bc=off"], ("ON", "OFF")),
            ([], ("ON", "ON")),
        ]:
            with self.subTest(flags=flags):
                cmake_defs = []

                def configure(args):
                    args.target = None  # build() resolves the native target before configuring.
                    cmake_defs.extend(generate(args))

                with patch.dict(main.__globals__, build=configure), patch.object(
                    sys, "argv", ["build.py", "build", "-t", "release", *flags]
                ):
                    main()
                for option, value in zip((DEBUG, BITCODE), expected):
                    self.assertIn(f"-D{option}={value}", cmake_defs)


if __name__ == "__main__":
    unittest.main()
