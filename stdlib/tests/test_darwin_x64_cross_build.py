"""Configure the stdlib build graph without compiling or requiring an Apple SDK."""
import importlib.util
import json
import logging
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


STDLIB = Path(__file__).resolve().parents[1]
TRIPLE = "x86_64-apple-darwin"
TUPLE = "darwin_x86_64_cjnative"
MACROS = ("unittest.testmacro", "unittest.mock.mockmacro", "deriving")


def write(path, contents=""):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(contents)


def write_xcrun(tools):
    path = tools / "xcrun"
    write(path, """#!/bin/sh
case "$*" in
    "--sdk macosx --show-sdk-path") echo "$FIXTURE_SDK";;
    "--sdk macosx --show-sdk-version") echo "$FIXTURE_VERSION"; exit "${FIXTURE_VERSION_RC:-0}";;
    *) echo "unsupported xcrun arguments: $*" >&2; exit 2;;
esac
""")
    path.chmod(0o755)


def run(*args, **kwargs):
    result = subprocess.run(args, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, **kwargs)
    if result.returncode:
        raise AssertionError(f"{args}: rc={result.returncode}\n{result.stdout}")
    return result.stdout


class BuildEntry(unittest.TestCase):
    def test_cross_build_and_install(self):
        spec = importlib.util.spec_from_file_location("stdlib_build", STDLIB / "build.py")
        build = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(build)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for compiler in ("clang", "clang++"):
                path = root / compiler
                path.touch()
                path.chmod(0o755)
            with (patch.object(build, "BUILD_DIR", tmp), patch.dict(os.environ),
                  patch.object(build, "LOG", logging.getLogger(__name__), create=True),
                  patch.object(build.subprocess, "Popen") as popen, patch.object(build, "log_output"),
                  patch.object(build, "IS_MACOS", True), patch.object(build, "IS_WINDOWS", False),
                  patch.object(build, "IS_ARM", True)):
                popen.return_value.returncode = 0
                with patch.object(sys, "argv", ["build.py", "build", "-t", "relwithdebinfo", "-j1",
                        "--target", "darwin-x86_64", "--target-toolchain", tmp, "--target-sysroot", "/sdk",
                        "--target-lib", "/rt", "--include", "/headers", "--local_mode_api"]):
                    build.main()
                configure, ninja = popen.call_args_list
                command = configure.args[0]
                directory = root / ("build-libs-" + TRIPLE)
                self.assertEqual(configure.kwargs["cwd"], str(directory))
                self.assertEqual(ninja.kwargs["cwd"], str(directory))
                defs = dict(arg[2:].split("=", 1) for arg in command if arg.startswith("-D"))
                self.assertEqual(defs["CMAKE_TOOLCHAIN_FILE"], "../../cmake/darwin_x86_64_toolchain.cmake")
                self.assertEqual(defs["CANGJIE_TARGET_TOOLCHAIN"], tmp)
                self.assertEqual(defs["CANGJIE_TARGET_SYSROOT"], "/sdk")
                self.assertEqual(defs["CANGJIE_TARGET_LIB"], "/rt")
                self.assertEqual(defs["CANGJIE_INCLUDE"], "/headers")
                self.assertEqual(defs["CANGJIE_ENABLE_LOCAL_MODE_API"], "ON")
                self.assertEqual(defs["CMAKE_BUILD_TYPE"], "RelWithDebInfo")
                self.assertEqual(configure.kwargs["env"]["CC"], str(root / "clang"))
                with patch.object(sys, "argv", ["build.py", "install", "--host", "darwin-x86_64"]):
                    build.main()
                self.assertEqual(popen.call_args.args[0], ["cmake", "--install", ".", "--prefix",
                                 str(STDLIB / ("output-" + TRIPLE))])
                self.assertEqual(popen.call_args.kwargs["cwd"], str(directory))
                with patch.object(sys, "argv", ["build.py", "install", "--prefix", str(root / "final-std")]):
                    build.main()
                self.assertEqual(popen.call_args.args[0][-2:], ["--prefix", str(root / "final-std")])
                self.assertEqual(popen.call_args.kwargs["cwd"], str(directory))


class CrossGraph(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        root = Path(cls.temp.name)
        src, sdk = root / "stdlib", root / "stage2"
        (src / "third_party").mkdir(parents=True)
        for rel in ("CMakeLists.txt", "cmake", "libs", "third_party/CMakeLists.txt", "third_party/cmake"):
            (src / rel).symlink_to(STDLIB / rel)
        # Supply configure-time inputs only. The nested PCRE project records
        # the real toolchain cache and installs fixtures for the regex rules.
        write(src / "third_party/boundscheck-v1.1.16/src/sample.c")
        pcre = src / "third_party/pcre2/pcre2"
        write(pcre / "CMakeLists.txt", """cmake_minimum_required(VERSION 3.16)
project(pcre_fixture C)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_library(pcre_probe OBJECT EXCLUDE_FROM_ALL pcre.c)
install(FILES libpcre2-8.a libpcre2-8.dylib DESTINATION lib)
install(FILES pcre2.h DESTINATION include)
""")
        write(pcre / "libpcre2-8.a", "!<arch>\n")
        write(pcre / "libpcre2-8.dylib")
        write(pcre / "pcre2.h")
        write(pcre / "pcre.c")
        for rel in ("schema/StdAstFormat.fbs", "include/cangjie/Basic/UGTypeKind.h",
                    "third_party/flatbuffers/bin/flatc", "third_party/flatbuffers/cangjie/fixture.cj",
                    "third_party/flatbuffers/include/flatbuffers/flatbuffers.h"):
            write(sdk / rel)
        tools = root / "tools"
        write_xcrun(tools)
        default_sdk = root / "XcodeDefault.sdk"
        default_sdk.mkdir()
        cls.archiver = shutil.which("ar")
        cls.builds = {}
        for name, toolchain, sdk_name in (("mac", "darwin_x86_64", "MacOSX.sdk"),
                                         ("ios", "ios_simulator_x86_64", "iPhoneSimulator.sdk")):
            sysroot = root / sdk_name
            sysroot.mkdir()
            if name == "mac":
                write(sysroot / "SDKSettings.json", json.dumps({"Version": "15.2"}))
            wrapper = root / (name + ".cmake")
            # Skip compiler identification and ABI probes; CMake still emits
            # real AppleClang compile, cjc, archiver, link and install rules.
            write(wrapper, f"""set(CMAKE_HOST_SYSTEM_NAME Darwin)
set(CMAKE_HOST_SYSTEM_PROCESSOR arm64)
foreach(lang C CXX)
    set(CMAKE_${{lang}}_COMPILER "${{CMAKE_COMMAND}}")
    set(CMAKE_${{lang}}_COMPILER_ID AppleClang)
    set(CMAKE_${{lang}}_COMPILER_ID_RUN TRUE)
    set(CMAKE_${{lang}}_COMPILER_VERSION 16.0)
    set(CMAKE_${{lang}}_COMPILER_FORCED TRUE)
endforeach()
set(CMAKE_AR "{cls.archiver}" CACHE FILEPATH "")
set(CMAKE_RANLIB "{shutil.which('ranlib')}" CACHE FILEPATH "")
set(CANGJIE_DSYMUTIL "${{CMAKE_COMMAND}}")
include("{STDLIB}/cmake/{toolchain}_toolchain.cmake")
""")
            build = root / name
            output = run("cmake", "-S", src, "-B", build, "-G", "Ninja",
                         f"-DCMAKE_TOOLCHAIN_FILE={wrapper}", f"-DCANGJIE_TARGET_TOOLCHAIN={tools}",
                         f"-DCANGJIE_TARGET_SYSROOT={sysroot}", "-DCANGJIE_SKIP_FIND_OPENSSL=ON",
                         "-DCMAKE_OSX_ARCHITECTURES=arm64", "-DCMAKE_BUILD_TYPE=Release",
                         env={**os.environ, "CANGJIE_HOME": str(sdk), "FIXTURE_SDK": str(default_sdk),
                              "FIXTURE_VERSION": "15.0", "FIXTURE_VERSION_RC": "0",
                              "PATH": str(tools) + os.pathsep + os.environ["PATH"]})
            cls.builds[name] = build, output
        cls.mac, cls.configure = cls.builds["mac"]
        cls.sysroot = root / "MacOSX.sdk"
        cls.install = (cls.mac / "libs/cmake_install.cmake").read_text()

    def commands(self, target):
        return run("ninja", "-C", self.mac, "-t", "commands", target).splitlines()

    def test_thin_native_targets_and_nested_pcre(self):
        self.assertIn(f"CROSS COMPILING libs from arm64-Darwin to {TRIPLE}", self.configure)
        for cache in ("CMakeCache.txt", "third_party/pcre2-build/CMakeCache.txt"):
            self.assertIn("CMAKE_OSX_ARCHITECTURES:STRING=x86_64\n", (self.mac / cache).read_text())
        pcre, = json.loads((self.mac / "third_party/pcre2-build/compile_commands.json").read_text())
        self.assertIn("-arch x86_64", pcre["command"])
        self.assertNotIn("-arch arm64", pcre["command"])
        entries = json.loads((self.mac / "compile_commands.json").read_text())
        for suffix in ("/string_SIMD.c", "/string_strstr.c", "/boundscheck-v1.1.16/src/sample.c"):
            commands = [e["command"] for e in entries if e["file"].endswith(suffix)]
            self.assertTrue(commands, suffix)
            for command in commands:
                self.assertIn("-arch x86_64", command)
                self.assertNotIn("-arch arm64", command)
                if "string_SIMD" in suffix:
                    self.assertIn("-mfma -mavx2", command)
        stdffi = self.commands("stdFFI")[-1]
        self.assertIn("-arch x86_64", stdffi)
        self.assertNotIn("-arch arm64", stdffi)

    def test_mac_macro_products(self):
        for package in MACROS:
            with self.subTest(package=package):
                source = "std." + package
                dylib = f"libcangjie-std-{package}.dylib"
                self.assertIn(f'/runtime/lib/{TUPLE}" TYPE FILE FILES "{self.mac}/lib/{TUPLE}/{dylib}"',
                              self.install)
                self.assertIn(f'/modules/{TUPLE}/std" TYPE FILE FILES "{self.mac}/modules/{TUPLE}/std/{source}.cjo"',
                              self.install)
                steps = self.commands("std-" + package)
                self.assertTrue(any(f"--target={TRIPLE}" in line and f"/{source}.a " in line for line in steps))
                self.assertTrue(any("-arch x86_64" in line and line.endswith("/" + dylib) for line in steps))

    def test_installed_macro_layout_after_relocation(self):
        # Run the generated file-install rules for the three packages only.
        # Payloads are markers; they test installation, not Mach-O contents.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            prefix = root / "prefix"
            rules = []
            for pkg in MACROS:
                dylib = f"libcangjie-std-{pkg}.dylib"
                cjo = f"std.{pkg}.cjo"
                write(self.mac / "lib" / TUPLE / dylib, pkg)
                write(self.mac / "modules" / TUPLE / "std" / cjo, cjo)
                rules.extend(line for line in self.install.splitlines() if "file(INSTALL " in line
                             and line.endswith((f'/{dylib}")', f'/{cjo}")')))
            # Include any alias install rule so a reintroduced (even broken) link is visible.
            rules.extend(line for line in self.install.splitlines() if "file(INSTALL " in line
                         and "/lib-macro_std." in line)
            script = root / "install.cmake"
            write(script, "\n".join(rules))
            run("cmake", f"-DCMAKE_INSTALL_PREFIX={prefix}", "-P", script, env={**os.environ, "DESTDIR": ""})
            relocated = root / "relocated"
            prefix.rename(relocated)
            modules = relocated / "modules" / TUPLE / "std"
            for pkg in MACROS:
                cjo = f"std.{pkg}.cjo"
                self.assertEqual((modules / cjo).read_text(), cjo)
                canonical = relocated / "runtime/lib" / TUPLE / f"libcangjie-std-{pkg}.dylib"
                self.assertEqual(canonical.read_text(), pkg)
            self.assertEqual(sorted(p.name for p in modules.glob("lib-macro_std.*.dylib")), [])

    def test_ios_keeps_native_only_macro_links_disabled(self):
        build, _ = self.builds["ios"]
        self.assertNotIn("lib-macro_std.", (build / "libs/cmake_install.cmake").read_text())
        targets = run("ninja", "-C", build, "-t", "targets", "all").splitlines()
        self.assertIn("std-core: phony", targets)
        for package in MACROS:
            self.assertFalse(f"std-{package}: phony" in targets, package)

    def test_macro_source_directories(self):
        for platform, expected in (("mac", True), ("ios", False)):
            install = (self.builds[platform][0] / "libs/std/cmake_install.cmake").read_text()
            self.assertIn("/std/unittest/cmake_install.cmake", install)
            self.assertEqual("/unittest/testmacro/cmake_install.cmake" in install, expected)

    def test_archive_extraction(self):
        steps = self.commands("cangjieCJNATIVECore")
        self.assertTrue(any(f"{self.archiver} x " in cmd and "/std.core.a" in cmd for cmd in steps))

    def test_archive_combination(self):
        steps = self.commands("cangjie-std-envFFI")
        self.assertTrue(any(f"MergeArchives.cmake ARCHIVER {self.archiver} " in cmd for cmd in steps))

    def test_direct_darwin_linker(self):
        link, = (line for line in self.commands("std-core") if "/ld64.lld " in line)
        self.assertIn("-arch x86_64", link)
        self.assertNotIn(" -B", link)
        self.assertNotIn(" --sysroot", link)

    def test_explicit_sdk_settings_reach_linker(self):
        link, = (line for line in self.commands("std-core") if "/ld64.lld " in line)
        self.assertIn(f"-syslibroot {self.sysroot} -platform_version macos 12.0.0 15.2 ", link)
        self.assertIn(f"CANGJIE_MACOSX_SDK_PATH: {self.sysroot}\n", self.configure)
        self.assertIn("CANGJIE_MACOSX_SDK_VERSION: 15.2\n", self.configure)


class SDKDiscovery(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.sdk = self.root / "MacOSX.sdk"
        self.sdk.mkdir()
        write_xcrun(self.root)
        write(self.root / "CMakeLists.txt", f"""cmake_minimum_required(VERSION 3.16.5)
project(sdk_fixture NONE)
include("{STDLIB}/cmake/modules/ReadDarwinSDKInfo.cmake")
file(WRITE "${{CMAKE_BINARY_DIR}}/sdk.txt" "${{CANGJIE_MACOSX_SDK_PATH}}\n${{CANGJIE_MACOSX_SDK_VERSION}}")
""")

    def configure(self, *definitions, **env):
        return subprocess.run(
            ["cmake", "-S", self.root, "-B", self.root / "build", *definitions],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            env={**os.environ, "PATH": str(self.root) + os.pathsep + os.environ["PATH"],
                 "FIXTURE_SDK": str(self.sdk), "FIXTURE_VERSION": "15.0", "FIXTURE_VERSION_RC": "0", **env})

    def test_default_sdk_uses_named_xcrun(self):
        write(self.sdk / "SDKSettings.json", '{"Version":"15.2"}')
        result = self.configure()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual((self.root / "build/sdk.txt").read_text().splitlines(), [str(self.sdk), "15.0"])

    def test_cmake_sysroot_falls_back_to_named_version(self):
        for settings in (None, '{}'):
            with self.subTest(settings=settings):
                if settings is not None:
                    write(self.sdk / "SDKSettings.json", settings)
                result = self.configure(f"-DCMAKE_SYSROOT={self.sdk}", FIXTURE_SDK="unused")
                self.assertEqual(result.returncode, 0, result.stdout)
                self.assertEqual((self.root / "build/sdk.txt").read_text().splitlines(), [str(self.sdk), "15.0"])

    def test_missing_sdk_stops_configuration(self):
        missing = str(self.root / "absent.sdk")
        for definitions, path, diagnostic in (
                ([], "", "--show-sdk-path failed"),
                ([], missing, "macOS SDK directory does not exist"),
                ([f"-DCANGJIE_TARGET_SYSROOT={missing}"], str(self.sdk), "macOS SDK directory does not exist")):
            with self.subTest(definitions=definitions, path=path):
                result = self.configure(*definitions, FIXTURE_SDK=path)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn(diagnostic, result.stdout)

    def test_unresolved_version_stops_configuration(self):
        for version, rc, diagnostic in (("", "0", "--show-sdk-version failed"),
                                        ("15.0", "1", "--show-sdk-version failed"),
                                        ("unknown", "0", "Invalid macOS SDK version")):
            with self.subTest(version=version, rc=rc):
                result = self.configure(f"-DCANGJIE_TARGET_SYSROOT={self.sdk}",
                                        FIXTURE_VERSION=version, FIXTURE_VERSION_RC=rc)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn(diagnostic, result.stdout)


if __name__ == "__main__":
    unittest.main()
