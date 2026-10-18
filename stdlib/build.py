#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

"""cangjie compiler build entry"""

import argparse
import logging
import json
import multiprocessing
import os
import platform
import shutil
import signal
import selectors
import time
import re
import stat
import subprocess
import sys
from enum import Enum
from logging.handlers import TimedRotatingFileHandler
from pathlib import Path
from subprocess import DEVNULL, PIPE

HOME_DIR = os.path.dirname(os.path.abspath(__file__))
BUILD_DIR = os.path.join(HOME_DIR, "build")
CMAKE_BUILD_DIR = os.path.join(BUILD_DIR, "build")
CMAKE_OUTPUT_DIR = os.path.join(HOME_DIR, "output")
LOG_DIR = os.path.join(BUILD_DIR, "logs")
LOG_FILE = os.path.join(LOG_DIR, "cangjie.log")

IS_WINDOWS = platform.system() == "Windows"
IS_MACOS = platform.system() == "Darwin"
IS_ARM = platform.uname().processor in ["aarch64", "arm", "arm64"]
# Wait for the version of aarch64 libcore to be ready.
MAKE_JOBS = multiprocessing.cpu_count() + 2

TARGET_DICTIONARY = {
    "native": None,
    "ohos-aarch64": "aarch64-linux-ohos",
    "ohos-x86_64": "x86_64-linux-ohos",
    "ohos-arm": "arm-linux-ohos",
    "windows-x86_64": "x86_64-w64-mingw32",
    "ios-simulator-aarch64": "arm64-apple-ios11-simulator",
    "ios-simulator-x86_64": "x86_64-apple-ios11-simulator",
    "ios-aarch64": "arm64-apple-ios11",
    "android-aarch64": "aarch64-linux-android23",
    "android23-aarch64": "aarch64-linux-android23",
    "android31-aarch64": "aarch64-linux-android31",
    "android26-aarch64": "aarch64-linux-android26",
    "android23-arm": "arm-linux-android23",
    "android-x86_64": "x86_64-linux-android",
    "android31-x86_64": "x86_64-linux-android31",
    "android26-x86_64": "x86_64-linux-android26"
}

def resolve_path(path):
    if os.path.isabs(path):
        return path
    return os.path.abspath(path)

def cmake_cache_value(cache_path, name):
    if not os.path.exists(cache_path):
        return None
    key = name + ":"
    with open(cache_path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if line.startswith(key) and "=" in line:
                return line.split("=", 1)[1].strip()
    return ""

def requested_target_lib(target_lib):
    return ";".join(target_lib) if target_lib else ""

def native_build_target(value):
    # Shared by Ninja and the Windows Make fallback. A target must not become
    # an option (e.g. -n/-t) or a Make variable assignment on either backend.
    if not isinstance(value, str) or not value.strip() or value.startswith("-") or "=" in value:
        raise argparse.ArgumentTypeError(
            "build target must be nonempty and must not be an option or variable assignment")
    return value

def stdlib_configure_required(cache_path, requested):
    if not os.path.exists(cache_path):
        return True
    return cmake_cache_value(cache_path, "CANGJIE_TARGET_LIB") != requested

def compiler_memory_budget(args):
    values = (args.memory_budget_gib, args.compiler_heap_gib, args.native_reserve_gib)
    if all(value is None for value in values):
        return ""
    if sys.platform != "linux":
        raise ValueError("compiler memory budgeting requires the Linux shared package pool")
    if any(value is None or value <= 0 for value in values):
        raise ValueError("memory-budget-gib, compiler-heap-gib and native-reserve-gib must all be positive")
    packages = min(args.jobs, args.package_jobs)
    if packages * args.compiler_heap_gib + args.native_reserve_gib > args.memory_budget_gib:
        raise ValueError("compiler heaps plus native reserve exceed memory-budget-gib")
    # The runtime's GB suffix is binary GiB. This is the managed heap limit;
    # the caller's memory scope also bounds native allocations and subprocesses.
    return str(args.compiler_heap_gib)

def run_logged_command(argv, cwd, budget=False, env=None):
    # The whole build/install worker is already inside the stable supervisor.
    # Keep the private jobserver descriptors; no cooperative idle/PGID owner.
    child = subprocess.Popen(argv, cwd=cwd, env=env, stdout=PIPE, close_fds=False)
    if sys.platform != 'linux':
        log_output(child)
        return child
    # A descendant can keep stdout open after the direct command dies. Observe
    # that command independently of EOF so our stable ancestor can take over.
    import selectors
    import fcntl
    fd = child.stdout.fileno()
    os.set_blocking(fd, False)
    pending = bytearray()
    def consume(data):
        pending.extend(data)
        while b'\n' in pending:
            line, _, rest = pending.partition(b'\n')
            pending[:] = rest
            LOG.info(line.decode('ascii', 'ignore').rstrip())
    with selectors.DefaultSelector() as selector:
        selector.register(fd, selectors.EVENT_READ)
        eof = False
        while child.poll() is None:
            if not eof and selector.select(0.05):
                try: data = os.read(fd, 65536)
                except BlockingIOError: continue
                if data: consume(data)
                else:
                    eof = True
                    selector.unregister(fd)
            elif eof:
                child.wait()
        # Preserve everything already buffered from a normally finished direct
        # child. A still-live orphan writer cannot extend this drain forever.
        remaining = fcntl.fcntl(fd, fcntl.F_GETPIPE_SZ)
        while remaining:
            try: data = os.read(fd, min(65536, remaining))
            except BlockingIOError: break
            if not data: break
            consume(data)
            remaining -= len(data)
    child.stdout.close()
    if pending: LOG.info(pending.decode('ascii', 'ignore').rstrip())
    if child.returncode != 0:
        LOG.error('build error: %d!', child.returncode)
        sys.exit(child.returncode if child.returncode > 0 else 128 - child.returncode)
    return child

def supervised_build_entry():
    helper = os.path.join(HOME_DIR, 'scripts', 'command-supervisor.mjs')
    owner_read, owner_write = os.pipe()
    os.set_inheritable(owner_read, True)
    # Only this Python frontend owns the writer. Node bridge and G inherit
    # the read endpoint; its EOF catches loss of Python even if Node lives.
    os.set_inheritable(owner_write, False)
    work_root = os.environ.get('CJCJ_SUPERVISOR_WORK', BUILD_DIR)
    os.makedirs(work_root, exist_ok=True)
    env = dict(os.environ, CJCJ_SUPERVISOR_OWNER_FD=str(owner_read),
               CJCJ_SUPERVISOR_WORK=os.path.abspath(work_root))
    active = None
    cancelled = None
    def interrupt(sig, _):
        nonlocal cancelled
        cancelled = cancelled or sig
        if active is not None:
            try: active.send_signal(cancelled)
            except ProcessLookupError: pass
    handlers = {sig: signal.signal(sig, interrupt) for sig in (signal.SIGINT, signal.SIGTERM)}
    try:
        active = subprocess.Popen(['node', helper, '--', sys.executable, '-I', __file__,
                                   '--supervised-build'] + sys.argv[1:], env=env, close_fds=False)
        if cancelled: interrupt(cancelled, None)
        rc = active.wait()
        return 128 + cancelled if cancelled else (rc if rc >= 0 else 128 - rc)
    finally:
        os.close(owner_write);os.close(owner_read)
        for sig, handler in handlers.items(): signal.signal(sig, handler)

def log_output(output, checker=None, args=[]):
    """log command output"""
    while True:
        line = output.stdout.readline()
        if not line:
            output.communicate()
            returncode = output.returncode
            if returncode != 0:
                LOG.error("build error: %d!\n", returncode)
                sys.exit(returncode if returncode > 0 else 128 - returncode)
            break
        try:
            LOG.info(line.decode("ascii", "ignore").rstrip())
        except UnicodeEncodeError:
            LOG.info(line.decode("utf-8", "ignore").rstrip())

def generate_cmake_defs(args):
    """convert args to cmake defs"""

    def bool_to_opt(value):
        return "ON" if value else "OFF"

    if args.target:
        if args.target == "aarch64-linux-ohos":
            toolchain_file = "ohos_aarch64_clang_toolchain.cmake"
        if args.target == "arm-linux-ohos":
            toolchain_file = "ohos_arm_clang_toolchain.cmake"
        elif args.target == "x86_64-linux-ohos":
            toolchain_file = "ohos_x86_64_clang_toolchain.cmake"
        elif args.target == "x86_64-w64-mingw32":
            toolchain_file = "mingw_x86_64_toolchain.cmake"
        elif args.target == "arm64-apple-ios11-simulator":
            toolchain_file = "ios_simulator_arm64_toolchain.cmake"
        elif args.target == "x86_64-apple-ios11-simulator":
            toolchain_file = "ios_simulator_x86_64_toolchain.cmake"
        elif args.target == "arm64-apple-ios11":
            toolchain_file = "ios_arm64_toolchain.cmake"
        elif "aarch64-linux-android" in args.target:
            toolchain_file = "android_aarch64_toolchain.cmake"
        elif "x86_64-linux-android" in args.target:
            toolchain_file = "android_x86_64_toolchain.cmake"
        elif "arm-linux-android" in args.target:
            toolchain_file = "android_arm_toolchain.cmake"
    else:
        args.target = None
        if IS_WINDOWS:
            toolchain_file = "mingw_x86_64_toolchain.cmake"
        elif IS_MACOS:
            toolchain_file = "darwin_aarch64_toolchain.cmake" if IS_ARM else "darwin_x86_64_toolchain.cmake"
        elif IS_ARM:
            toolchain_file = "linux_aarch64_toolchain.cmake"
        else:
            toolchain_file = "linux_x86_64_toolchain.cmake"

    if args.target:
        fields = args.target.split("-")

    install_prefix = CMAKE_OUTPUT_DIR

    result = [
        "-DCMAKE_BUILD_TYPE=" + args.build_type.value,
        "-DCMAKE_TOOLCHAIN_FILE=../../cmake/" + toolchain_file,
        "-DCMAKE_INSTALL_PREFIX=" + install_prefix,
        "-DCANGJIE_TARGET_LIB=" + (";".join(args.target_lib) if args.target_lib else ""),
        "-DCANGJIE_TARGET_TOOLCHAIN=" + (args.target_toolchain if args.target_toolchain else ""),
        "-DCANGJIE_ENABLE_HWASAN=" + bool_to_opt(args.hwasan),
        "-DCANGJIE_TARGET_SYSROOT=" + (args.target_sysroot if args.target_sysroot else ""),
        "-DCANGJIE_BUILD_ARGS=" + (";".join(args.build_args) if args.build_args else ""),
        "-DCANGJIE_INCLUDE=" + (";".join(args.include) if args.include else ""),
        "-DCANGJIE_BUILD_STDLIB_WITH_COVERAGE=" + bool_to_opt(args.stdlib_coverage),
        "-DCANGJIE_ENABLE_ASAN_COV=" + bool_to_opt(args.asancov),
        "-DBUILD_GCC_TOOLCHAIN=" + (args.gcc_toolchain if args.gcc_toolchain and args.target is None else ""),
    ]

    if args.target and "aarch64-linux-android" in args.target:
        android_api_level = re.match(r'aarch64-linux-android(\d{2})?', args.target).group(1)
        result.append("-DCMAKE_ANDROID_NDK=" + os.path.join(args.target_toolchain, "../../../../.."))
        result.append("-DCMAKE_ANDROID_API=" + (android_api_level if android_api_level else ""))

    if args.target and "arm-linux-android" in args.target:
        android_api_level = "23"
        result.append("-DCMAKE_ANDROID_NDK=" + os.path.join(args.target_toolchain, "../../../../.."))
        result.append("-DCMAKE_ANDROID_API=" + (android_api_level if android_api_level else ""))

    if args.sanitizer_support:
        result.append("-DCANGJIE_SANITIZER_SUPPORT=" + args.sanitizer_support)
    return result

def build(args):

    if args.target:
        args.target = TARGET_DICTIONARY[args.target]

    check_compiler(args)

    if args.gcc_toolchain and args.target:
        LOG.warning("--gcc-toolchain won't take effect when --target is specified, "
                     "because HOST and TARGET need different toolchains in cross compilation")

    """build cangjie compiler"""
    LOG.info("begin build...")

    if args.target == "aarch64-linux-ohos" or args.target == "x86_64-linux-ohos" or args.target == "arm-linux-ohos":
        # Frontend supports cross compilation in a general way by asking path to required tools
        # and libraries. However, Runtime supports cross compilation in a speific way, which asks
        # for the root path of OHOS toolchain. Since we asked for a path to tools, the root path of
        # OHOS toolchain is relative to the tool path we get. Tool path normally looks like
        # ${OHOS_ROOT}/prebuilts/clang/ohos/linux-x86_64/llvm/bin/. Six /.. can bring us to the root.
        os.environ["OHOS_ROOT"] = os.path.join(args.target_toolchain, "../../../../../..")

    if args.target and "linux-android" in args.target:
        os.environ["ANDROID_NDK_ROOT"] = os.path.join(args.target_toolchain, "../../../../..")

    if IS_MACOS:
        os.environ["ZERO_AR_DATE"] = "1"

    if sys.platform == "linux":
        if args.jobs < 0 or args.package_jobs < 1:
            raise ValueError("jobs must be nonnegative and package-jobs must be positive")
        if args.jobs == 0:
            args.jobs = len(os.sched_getaffinity(0))
    elif args.jobs <= 0 or args.jobs > 8:
        args.jobs = 8

    compiler_heap = compiler_memory_budget(args)
    LOG.info("STDLIB_BUILD_BUDGET %s", json.dumps({
        "cpuJobs": args.jobs, "packageJobs": min(args.jobs, args.package_jobs),
        "memoryBudgetGiB": args.memory_budget_gib, "compilerHeapGiB": args.compiler_heap_gib,
        "nativeReserveGiB": args.native_reserve_gib,
    }, sort_keys=True))

    if IS_WINDOWS:
        # For Windows, try Ninja first, otherwise use Make instead.
        ninja_valid = True
        try:
            output = subprocess.Popen(["ninja", "--version"], stdout=DEVNULL, stderr=DEVNULL)
            output.communicate()
            ninja_valid = output.returncode == 0
        except:
            ninja_valid = False

        if ninja_valid:
            generator = "Ninja"
            build_cmd = ["ninja"]
            if args.jobs > 0:
                build_cmd.extend(["-j", str(args.jobs)])
        else:
            generator = "MinGW Makefiles"
            build_cmd = ["mingw32-make", "-j" + str(MAKE_JOBS)]
    else:
        # For Linux, just use Ninja.
        generator = "Ninja"
        build_cmd = ["ninja"]
        if args.jobs > 0:
            build_cmd.extend(["-j", str(args.jobs)])

    cmake_command = ["cmake", HOME_DIR, "-G", generator] + generate_cmake_defs(args)
    # Native Ninja targets allow bounded dependency builds without replacing
    # the stdlib top-level graph. The default still builds its complete graph.
    if args.build_targets:
        build_cmd.extend(args.build_targets)
    if sys.platform == "linux":
        cmake_command += ["-DCANGJIE_SHARED_JOBSERVER=ON",
                          "-DCANGJIE_CPU_JOBS=" + str(args.jobs),
                          "-DCANGJIE_PACKAGE_JOBS=" + str(min(args.jobs, args.package_jobs)),
                          "-DCANGJIE_COMPILER_HEAP_GIB=" + compiler_heap]
        build_cmd = ["node", os.path.join(HOME_DIR, "scripts", "ninja-jobserver.mjs"),
                     "run", "--jobs", str(args.jobs), "--"] + build_cmd

    if not os.path.exists(BUILD_DIR):
        os.makedirs(BUILD_DIR)

    if args.sanitizer_support:
        cmake_build_dir = os.path.join(BUILD_DIR, "build-libs-{}".format(args.sanitizer_support))
        if args.target:
            cmake_build_dir += "-{}".format(args.target)
    else:
        cmake_build_dir = os.path.join(BUILD_DIR, "build-libs-{}".format(args.target)) if args.target else CMAKE_BUILD_DIR

    cache_path = os.path.join(cmake_build_dir, "CMakeCache.txt")
    if not os.path.exists(cmake_build_dir):
        os.makedirs(cmake_build_dir)
    budget_changed = sys.platform == "linux" and any(
        cmake_cache_value(cache_path, key) != value for key, value in {
            "CANGJIE_SHARED_JOBSERVER": "ON", "CANGJIE_CPU_JOBS": str(args.jobs),
            "CANGJIE_PACKAGE_JOBS": str(min(args.jobs, args.package_jobs)),
            "CANGJIE_COMPILER_HEAP_GIB": compiler_heap}.items())
    if budget_changed or stdlib_configure_required(cache_path, requested_target_lib(args.target_lib)):
        # Configuration runs before the Ninja pool exists and owns only the
        # caller's implicit slot. Its native dependency builds (e.g. PCRE2)
        # must stay serial even when the inherited build default is much wider.
        configure_env = dict(os.environ, CMAKE_BUILD_PARALLEL_LEVEL='1')
        run_logged_command(cmake_command, cmake_build_dir, env=configure_env)

    # Keep only descriptors the invoking make/jobserver deliberately made inheritable.
    output = run_logged_command(build_cmd, cmake_build_dir, budget=sys.platform == 'linux')

    if output.returncode != 0:
        LOG.fatal("build failed")

    LOG.info("end build")

def install(args):
    """install targets"""
    LOG.info("begin install targets...")
    
    if args.host:
        args.host = TARGET_DICTIONARY[args.host]

    targets = []

    # - If "build.py install" is invoked without "--host",
    #   the native build directories and all cross-compiled libs
    #   will be installed to the "output" directory.
    # - If "build.py install" is invoked with "--host <triple>",
    #   the build-cjc-<triple> directories and all cross-compiled libs
    #   will be installed to a seperated "output-<target>" directory.

    # Searching for cjc's build directory.
    if not args.host:
        if os.path.exists(CMAKE_BUILD_DIR):
            targets.append(("native", CMAKE_BUILD_DIR))
    else:
        suffix = "cjc-{}".format(args.host)
        cross_build_path = os.path.join(BUILD_DIR, "build-{}".format(suffix))
        if os.path.exists(cross_build_path):
            targets.append((suffix, cross_build_path))

    # Searching for all libs' build directories.
    for directory in os.listdir(BUILD_DIR):
        build_prefix = "build-libs-"
        if directory.startswith(build_prefix):
            targets.append(("libs-{}".format(directory[len(build_prefix):]), os.path.join(BUILD_DIR, directory)))

    if len(targets) == 0:
        LOG.fatal("Nothing is built yet.")
        sys.exit(1)

    # install for all build directories in the list
    for target in targets:
        LOG.info("installing {} build...".format(target[0]))
        cmake_cmd = ["cmake", "--install", "."]
        if args.prefix:
            cmake_cmd += ["--prefix", os.path.abspath(args.prefix)]
        elif args.host:
            cmake_cmd += ["--prefix", os.path.join(HOME_DIR, "output-{}".format(args.host))]
        LOG.info(target[1])
        output = run_logged_command(cmake_cmd, target[1])
        if output.returncode != 0:
            LOG.fatal("install {} build failed".format(target[0]))
            sys.exit(1)

    if args.host == "x86_64-w64-mingw32":
        mingw_bin_path = os.path.join(HOME_DIR, "output-x86_64-w64-mingw32/third_party/mingw/bin")
        if os.path.exists(mingw_bin_path):
            for bin in os.listdir(mingw_bin_path):
                bin_path = os.path.join(mingw_bin_path, bin)
                if os.path.isfile(bin_path) and not bin.endswith(".exe"):
                    os.remove(bin_path)
    LOG.info("end install targets...")


def redo_with_write(redo_func, path, err):

    # Is the error an access error?
    if not os.access(path, os.W_OK):
        os.chmod(path, stat.S_IWUSR)
        redo_func(path)
    else:
        raise

def clean(args):
    """clean build outputs and logs"""
    LOG.info("begin clean...\n")
    handlerToBeRemoved = []
    for handler in LOG.handlers:
        if isinstance(handler, logging.FileHandler):
            handler.close()
            handlerToBeRemoved.append(handler)
    for handler in handlerToBeRemoved:
        LOG.removeHandler(handler)
    output_dirs = []
    # Remove entire build directory by default
    output_dirs.append("build")
    output_dirs.append("output")
    # Remove flatc-generated AST format sources
    output_dirs.append("libs/std/ast/StdAstFormat_generated.cj")
    output_dirs.append("libs/std/ast/astformat_generated.cj")
    for file_path in output_dirs:
        abs_file_path = os.path.join(HOME_DIR, file_path)
        if os.path.isdir(abs_file_path):
            # If some build files are read-only and not allowed to be deleted (especially in Windows),
            # Try to deal with the error by giving write permissions and deleting them again.
            shutil.rmtree(abs_file_path, ignore_errors=False, onerror=redo_with_write)
        if os.path.isfile(abs_file_path):
            os.remove(abs_file_path)
    LOG.info("end clean\n")


def init_log(name):
    """init log config"""
    if not os.path.exists(LOG_DIR):
        os.makedirs(LOG_DIR)

    log = logging.getLogger(name)
    log.setLevel(logging.DEBUG)
    formatter = logging.Formatter(
        "[%(asctime)s] [%(module)s] [%(levelname)s] %(message)s",
        "%Y-%m-%d %H:%M:%S"
    )
    streamhandler = logging.StreamHandler(sys.stdout)
    streamhandler.setLevel(logging.DEBUG)
    streamhandler.setFormatter(formatter)
    log.addHandler(streamhandler)
    filehandler = TimedRotatingFileHandler(
        LOG_FILE, when="W6", interval=1, backupCount=60
    )
    filehandler.setLevel(logging.DEBUG)
    filehandler.setFormatter(formatter)
    log.addHandler(filehandler)
    return log


class BuildType(Enum):
    """CMAKE_BUILD_TYPE options"""

    debug = "Debug"
    release = "Release"
    relwithdebinfo = "RelWithDebInfo"

    def __str__(self):
        return self.name

    def __repr__(self):
        return str(self)

    @staticmethod
    def argparse(s):
        try:
            return BuildType[s]
        except KeyError:
            return s.build_type


def main():
    """build entry"""
    parser = argparse.ArgumentParser(description="build cangjie project")
    subparsers = parser.add_subparsers(help="sub command help")
    parser_build = subparsers.add_parser("build", help=" build cangjie")
    parser_build.add_argument(
        "-t",
        "--build-type",
        type=BuildType.argparse,
        dest="build_type",
        choices=list(BuildType),
        required=True,
        help="select target build type",
    )
    parser_build.add_argument(
        "--jobs","-j" , dest="jobs", type=int, default=0, help="run N jobs in parallel (0 means default)"
    )
    parser_build.add_argument(
        "--package-jobs", type=int, default=2,
        help="maximum simultaneous memory-heavy cjc packages (CPU tokens are shared separately)"
    )
    parser_build.add_argument("--memory-budget-gib", type=int,
        help="total memory scope in GiB; requires compiler-heap-gib and native-reserve-gib")
    parser_build.add_argument("--compiler-heap-gib", type=int,
        help="managed heap limit for each static/BC compiler process in GiB")
    parser_build.add_argument("--native-reserve-gib", type=int,
        help="memory reserved for native tools and non-heap allocations in GiB")
    parser_build.add_argument(
        "--build-target", dest="build_targets", type=native_build_target, action="append", default=[],
        help="build a named native target and its dependencies (repeatable; default all)"
    )
    parser_build.add_argument(
        "--hwasan", action="store_true", help="build with hardware asan"
    )
    parser_build.add_argument(
        "--target", dest="target", type=str, choices=TARGET_DICTIONARY.keys(), default="native",
        help="build a second stdlib for the target triple specified"
    )
    parser_build.add_argument(
        "--target-lib", "-L", dest="target_lib", type=str, action='append', default=[],
        help="link libraries under this path for all products"
    )
    parser_build.add_argument(
        "--target-toolchain", dest="target_toolchain", type=str,
        help="use the tools under the given path to cross-compile stdlib"
    )
    parser_build.add_argument(
        "--include", "-I", dest="include", type=str, action='append', default=[],
        help="search header files in given paths"
    )
    parser_build.add_argument(
        "--stdlib-coverage", action="store_true", help="build stdlib with coverage"
    )
    parser_build.add_argument(
        "--target-sysroot", dest="target_sysroot", type=str,
        help="pass this argument to C/CXX compiler as --sysroot"
    )
    parser_build.add_argument(
        "--asancov", action="store_true", help="build with asan and sanitize-coverage, used for cjc_fuzz and lsp_test"
    )
    parser_build.add_argument(
        "--cjlib-sanitizer-support",
        type=str,
        choices=["asan", "tsan", "hwasan"],
        dest="sanitizer_support",
        help="Enable cangjie sanitizer support for cangjie libraries."
    )
    parser_build.add_argument(
        "--build-args",
        type=str,
        dest="build_args",
        action='append', default=[],
        help="ther arguments directly passed to cjc"
    )
    parser_build.add_argument(
        "--gcc-toolchain", dest="gcc_toolchain", help="Specify GCC toolchain for Clang to use"
    )
    parser_build.set_defaults(func=build)

    parser_install = subparsers.add_parser("install", help="install targets")

    parser_install.add_argument(
        "--host", dest="host", type=str, choices=TARGET_DICTIONARY.keys(), help="Generate installation package for the host"
    )
    parser_install.add_argument(
        "--prefix", dest="prefix", help="target install prefix"
    )
    parser_install.set_defaults(func=install)

    parser_clean = subparsers.add_parser("clean", help="clean build")
    parser_clean.set_defaults(func=clean)

    args, unknown = parser.parse_known_args()
    if args.func == build:
        # Some argparse versions remove a literal '--' value before invoking
        # type conversion. Validate the final values before any platform branch.
        try:
            args.build_targets = [native_build_target(value) for value in args.build_targets]
        except argparse.ArgumentTypeError as error:
            parser_build.error("argument --build-target: " + str(error))
    if sys.platform == 'linux' and args.func in (build, install) and not SUPERVISED_BUILD:
        sys.exit(supervised_build_entry())
    args.func(args)

def check_compiler(args):
    # If user didn't specify --target-toolchain, we search for an available compiler in $PATH.
    # If user did specify --target-toolchain, we search in user given path ONLY. By doing so
    # user could see a 'compiler not found' error if the given path is incorrect.
    toolchain_path = args.target_toolchain if args.target_toolchain else None
    if toolchain_path and (not os.path.exists(toolchain_path)):
        LOG.error("the given toolchain path does not exist, {}".format(toolchain_path))
    # The gcc with the MinGW triple prefix is used for Windows native compiling.
    if IS_WINDOWS and args.target is None:
        c_compiler = shutil.which("x86_64-w64-mingw32-gcc.exe", path=toolchain_path)
        cxx_compiler = shutil.which("x86_64-w64-mingw32-g++.exe", path=toolchain_path)
    else: # On other platform, clang is always the first choice.
        c_compiler = shutil.which("clang", path=toolchain_path)
        cxx_compiler = shutil.which("clang++", path=toolchain_path)
    # If clang is not available and we are cross compiling, we check for gcc cross compiler.
    if (c_compiler == None or cxx_compiler == None) and args.target:
        prefix = ""
        if args.target == "windows-x86_64":
            prefix = "x86_64-w64-mingw32"
        c_compiler = shutil.which(prefix + "-gcc", path=toolchain_path)
        cxx_compiler = shutil.which(prefix + "-g++", path=toolchain_path)
    # If none of above is available, we search for generic gcc compiler.

    if c_compiler == None or cxx_compiler == None:
        c_compiler = shutil.which("gcc", path=toolchain_path)
        cxx_compiler = shutil.which("g++", path=toolchain_path)

    if c_compiler == None or cxx_compiler == None:
        if toolchain_path:
            LOG.error("cannot find available compiler in the given toolchain path")
        else:
            LOG.error("cannot find available compiler in $PATH")
        LOG.error("clang/clang++ or gcc/g++ is required to build cangjie compiler")

    os.environ["CC"] = c_compiler
    os.environ["CXX"] = cxx_compiler

if __name__ == "__main__":
    SUPERVISED_BUILD = len(sys.argv) > 1 and sys.argv[1] == '--supervised-build'
    if SUPERVISED_BUILD:
        if os.environ.get('CJCJ_SUPERVISOR_PARENT') != str(os.getppid()):
            raise RuntimeError('BUILD_WORKER_PARENT')
        del sys.argv[1]
    LOG = init_log("root")
    os.environ["LANG"] = "C.UTF-8"

    main()
