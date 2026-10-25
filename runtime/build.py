#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

import os
import sys
import subprocess
import platform
import re
import shutil
import argparse

# Get the directory where the script is located as the execution directory
script_path = os.path.dirname(os.path.abspath(__file__))


def nonnegative_jobs(value):
    try:
        jobs = int(value)
        if jobs < 0:
            raise ValueError
        return jobs
    except ValueError:
        raise argparse.ArgumentTypeError("jobs must be a nonnegative integer (0 means default)")


def effective_cpu_count():
    """CPU availability, including affinity and visible ancestor cgroup quotas."""
    count_fn = getattr(os, "process_cpu_count", os.cpu_count)
    limits = [count_fn() or 1]
    if hasattr(os, "sched_getaffinity"):
        try:
            limits.append(len(os.sched_getaffinity(0)) or 1)
        except OSError:
            pass
    if sys.platform == "linux":
        try:
            with open("/proc/self/cgroup") as stream:
                groups = [line.strip().split(":", 2) for line in stream]
            with open("/proc/self/mountinfo") as stream:
                mounts = [line.split() for line in stream]
            for mount in mounts:
                separator = mount.index("-")
                kind = mount[separator + 1]
                if kind not in ("cgroup", "cgroup2"):
                    continue
                # mountinfo encodes spaces, tabs, newlines and backslashes in octal.
                decode = lambda text: re.sub(r"\\([0-7]{3})", lambda m: chr(int(m[1], 8)), text)
                mount_root, mount_path = map(decode, mount[3:5])
                for _, controllers, group_path in groups:
                    if kind == "cgroup2" and controllers:
                        continue
                    if kind == "cgroup" and ("cpu" not in controllers.split(",") or
                                              "cpu" not in mount[separator + 3].split(",")):
                        continue
                    # Namespace '..' components denote hidden ancestors, not
                    # filesystem traversal. These interfaces omit the child names
                    # needed to map an inherited mount back to our current group.
                    root_parts = [part for part in mount_root.split("/") if part]
                    group_parts = [part for part in group_path.split("/") if part]
                    if (".." in root_parts or ".." in group_parts or
                            group_parts[:len(root_parts)] != root_parts):
                        print("Warning: cannot uniquely map CPU cgroup namespace "
                              f"membership {group_path!r} to mount root {mount_root!r}; "
                              "defaulting to 1 job. Set --jobs or an explicit build budget "
                              "to override.", file=sys.stderr)
                        limits.append(1)
                        continue
                    directory = os.path.join(mount_path, *group_parts[len(root_parts):])
                    while True:
                        try:
                            if kind == "cgroup2":
                                with open(os.path.join(directory, "cpu.max")) as stream:
                                    quota, period = stream.read().split()
                            else:
                                with open(os.path.join(directory, "cpu.cfs_quota_us")) as stream:
                                    quota = stream.read().strip()
                                with open(os.path.join(directory, "cpu.cfs_period_us")) as stream:
                                    period = stream.read().strip()
                            if quota != "max" and int(quota) > 0 and int(period) > 0:
                                limits.append(max(1, int(quota) // int(period)))
                        except (OSError, ValueError):
                            pass
                        if directory == mount_path:
                            break
                        directory = os.path.dirname(directory)
        except (OSError, ValueError):
            pass
    return max(1, min(limits))


def resolve_build_jobs(jobs):
    """Explicit CLI wins; otherwise consume the first declared parent budget."""
    if jobs > 0:
        return jobs
    for name in ("CANGJIE_BUILD_JOBS", "CMAKE_BUILD_PARALLEL_LEVEL", "KKK2_JOBS"):
        value = os.environ.get(name)
        if value:
            try:
                budget = int(value)
                if budget > 0:
                    return budget
            except ValueError:
                pass
            raise ValueError(f"{name} must be a positive integer")
    return effective_cpu_count()


def build_environment(jobs):
    env = os.environ.copy()
    for name in ("CANGJIE_BUILD_JOBS", "CMAKE_BUILD_PARALLEL_LEVEL", "KKK2_JOBS"):
        env[name] = str(jobs)
    return env

def do_clean(args):
    # Remove specified directories
    dirs_to_remove = ["output", "CMakebuild", "build/cjthread_build"]
    for directory in dirs_to_remove:
        try:
            subprocess.run(["rm", "-rf", os.path.join(script_path, directory)], check=True)
        except subprocess.CalledProcessError as e:
            print(f"Error cleaning directory {directory}: {e}")
            sys.exit(1)
    print("Cleaned successfully.")
    sys.exit(0)

def do_build(args):
    # Get script arguments
    target_args = args.target
    if target_args == "android-aarch64":
        target_args = "android23-aarch64"
    mode = args.build_type
    version = args.version
    prefix_path = args.prefix

    if target_args == 'native':
        target_platform = platform.system().lower()
    else:
        target_platform = target_args.rsplit('-', 1)[0]

    # Validate the version number parameter
    if not version:
        print("Parameter 3 - version number is empty, please enter the correct version number")
        sys.exit(1)

    # Define paths
    host_arch = platform.machine()
    host_os_name = platform.system().lower()

    # Adjust CMAKE_INSTALL_PREFIX based on target_args
    # Android API 23 is the baseline (no version suffix); 26/31 carry the suffix; ARM keeps 23.
    if target_platform == "ohos":
        install_prefix = os.path.join(prefix_path, f"linux_ohos_{mode}")
    elif target_platform == "android23":
        if target_args == "android23-arm":
            # ARM keeps the 23 suffix
            install_prefix = os.path.join(prefix_path, f"linux_android23_{mode}")
        else:
            # aarch64/x86_64 baseline
            install_prefix = os.path.join(prefix_path, f"linux_android_{mode}")
    elif target_platform == "android26":
        install_prefix = os.path.join(prefix_path, f"linux_android26_{mode}")
    elif target_platform == "android31" or target_platform == "android":
        install_prefix = os.path.join(prefix_path, f"linux_android31_{mode}")
    elif target_platform == "ios-simulator":
        install_prefix = os.path.join(prefix_path, f"ios_simulator_{mode}")
    else:
        install_prefix = os.path.join(prefix_path, f"{target_platform}_{mode}")

    # The configured output directory is configuration-specific.  Keep earlier
    # configurations intact; only the CMake work directory is single-use here.
    cmakebuild_dir = os.path.join(script_path, "CMakebuild")
    try:
        if os.path.exists(cmakebuild_dir):
            shutil.rmtree(cmakebuild_dir)
        print(f"Removed {cmakebuild_dir} directory successfully.")
    except Exception as e:
        print(f"Error removing {cmakebuild_dir} directory: {e}")
        sys.exit(1)

    if target_args in ('native'):
        target_arch = host_arch
    elif target_args in ('ohos-x86_64', 'ohos-aarch64', 'ohos-arm', 'windows-x86_64',
                         'android-x86_64', 'android-aarch64', 'android23-aarch64', 'android26-aarch64', 'android31-aarch64', 'android23-arm',
                         'ios-aarch64', 'ios-simulator-aarch64', 'ios-simulator-x86_64'):
        target_arch = target_args.rsplit('-', 1)[1]
    else:
        target_arch = None

    target_arch = target_arch.lower()
    if target_arch == "arm64":
        target_arch = "aarch64"
    elif target_arch == "arm32":
        target_arch = "arm"
    elif target_arch == "amd64":
        target_arch = "x86_64"

    shared_linker_flags = ""
    exe_linker_flags = ""
    if target_platform in ('linux'):
        shared_linker_flags = "-Wl,--disable-new-dtags,-rpath=\\$ORIGIN"
        exe_linker_flags = "-Wl,--disable-new-dtags,-rpath=\\$ORIGIN/../../runtime/lib/linux_${CMAKE_SYSTEM_PROCESSOR}_cjnative"
    elif target_platform in ('android', 'android23', 'android26', 'android31'):
        # android 64-bit devices may have 16 KB pages, 32-bit ARM uses 4 KB;
        # make sure the shared object is laid out accordingly.  linker default
        # is 4 KB on x86_64 and 64 KB on aarch64/arm.
        android_max_page_size = 4096 if target_arch == 'arm' else 16384
        # %-formatting is used on purpose: the rpath below contains a literal
        # ${CMAKE_SYSTEM_PROCESSOR} that str.format() would try to substitute.
        max_page_size_flag = "-Wl,-z,max-page-size=%d" % android_max_page_size
        shared_linker_flags = "-Wl,--disable-new-dtags,-rpath=\\$ORIGIN " + max_page_size_flag
        exe_linker_flags = ("-Wl,--disable-new-dtags,-rpath=\\$ORIGIN/../../runtime/lib/"
                            "linux_${CMAKE_SYSTEM_PROCESSOR}_cjnative " + max_page_size_flag)
    elif target_platform in ('darwin'):
        shared_linker_flags = "-Wl,-rpath,@loader_path"
        exe_linker_flags = "-Wl,-rpath,@loader_path/../../runtime/lib/darwin_${CMAKE_SYSTEM_PROCESSOR}_cjnative"
    elif target_platform in ('windows'):
        shared_linker_flags = "-Wl,--no-insert-timestamp"
        exe_linker_flags = "-Wl,--no-insert-timestamp"

    mode = mode.capitalize()
    macos_flag = 1 if platform.system().lower() == 'darwin' else 0
    if macos_flag:
        os.environ["ZERO_AR_DATE"] = "1"
    hwasan_flag = 1 if args.hwasan == True else 0
    euler_flag = 1 if args.euler_flag == True else 0
    # Perform different build actions based on the target_args
    if target_args == "native":
        cmake_command = [
            "cmake",
            "-DCMAKE_INSTALL_PREFIX={}_{}".format(install_prefix, target_arch),
            "-DCMAKE_BUILD_TYPE={}".format(mode),
            "-DCOPYGC_FLAG=1",
            "-DDOPRA_FLAG=1",
            "-DMRT_GC_UNIT_OHOS_HOST={}".format("ON" if args.gc_unit_ohos_host else "OFF"),
            "-DMRT_TESTABLE_INTERNALS={}".format("ON" if args.testable_internals else "OFF"),
            "-DOHOS_FLAG=0",
            "-DANDROID_FLAG=0",
            "-DIOS_FLAG=0",
            "-DIOS_SIMULATOR_FLAG=0",
            "-DEULER_FLAG={}".format(euler_flag),
            "-DMACOS_FLAG={}".format(macos_flag),
            "-DRUNTIME_TRACE_FLAG=1",
            "-DASAN_FLAG=0",
            "-DHWASAN_FLAG={}".format(hwasan_flag),
            "-DSANITIZER_SUPPORT={}".format(args.sanitizer_support),
            "-DCOV=0",
            "-DDUMPADDRESS_FLAG=0",
            "-DCJ_SDK_VERSION={}".format(version),
            "-DDISABLE_VERSION_CHECK=1",
            "-DCMAKE_C_COMPILER=clang",
            "-DCMAKE_CXX_COMPILER=clang++",
            "-DCMAKE_AR_PATH=ar",
            "-DCMAKE_SHARED_LINKER_FLAGS={}".format(shared_linker_flags),
            "-DCMAKE_EXE_LINKER_FLAGS={}".format(exe_linker_flags),
            "-S", ".", "-B", "CMakebuild"
        ]
        build_target(cmake_command, args)

    elif target_args == "windows-x86_64":
        if args.target_toolchain == None:
            print("Please configure windows toolchain, for example '/opt/buildtools/mingw-w64-v11.0.1'")
            sys.exit(1)
        if args.sanitizer_support:
            print("Windows does not support sanitizer support")
            sys.exit(1)
        # The host executes CMake and Python, even for a Windows cross target.
        # Native Windows needs ';'; ':' corrupts drive-letter PATH entries and
        # can expose the MSYS Python to CMake's native Windows script paths.
        os.environ["PATH"] = os.path.join(args.target_toolchain, "bin") + os.pathsep + os.environ["PATH"]
        cmake_command = [
            "cmake",
            "-DCMAKE_INSTALL_PREFIX={}_{}".format(install_prefix, target_arch),
            "-DCMAKE_BUILD_TYPE={}".format(mode),
            "-DCOPYGC_FLAG=1",
            "-DDOPRA_FLAG=1",
            "-DOHOS_FLAG=0",
            "-DANDROID_FLAG=0",
            "-DIOS_FLAG=0",
            "-DIOS_SIMULATOR_FLAG=0",
            "-DEULER_FLAG=0",
            "-DWINDOWS_FLAG=1",
            "-DDSU_FLAG=0",
            "-DRUNTIME_TRACE_FLAG=1",
            "-DASAN_FLAG=0",
            "-DHWASAN_FLAG={}".format(hwasan_flag),
            "-DSANITIZER_SUPPORT=0",
            "-DCOV=0",
            "-DDUMPADDRESS_FLAG=0",
            "-DCJ_SDK_VERSION={}".format(version),
            "-DDISABLE_VERSION_CHECK=1",
            "-DCMAKE_C_COMPILER=clang",
            "-DCMAKE_CXX_COMPILER=clang++",
            "-DCMAKE_AR_PATH=ar",
            "-DCMAKE_SHARED_LINKER_FLAGS={}".format(shared_linker_flags),
            "-DCMAKE_EXE_LINKER_FLAGS={}".format(exe_linker_flags),
            "-S", ".", "-B", "CMakebuild"
        ]
        build_target(cmake_command, args)

    elif target_args in ["ohos-aarch64", "ohos-arm", "ohos-x86_64"]:
        if args.target_toolchain is None and args.ohos_public_sdk is None:
            print("Please configure ohos toolchain, for example '/root/workspace/ohos_dep_files/'")
            sys.exit(1)
        if target_args == "ohos-aarch64":
            ohos_flag = "1"
            target_arch = "aarch64"
        elif target_args == "ohos-x86_64":
            ohos_flag = "2"
            target_arch = "x86_64"
        elif target_args == "ohos-arm":
            ohos_flag = "3"
            target_arch = "arm"
        ptrauth_flags = [
            "-DRUNTIME_FORWARD_PTRAUTH_CFI=1",
            "-DRUNTIME_BACKWARD_PTRAUTH_CFI=1",
        ] if target_args == "ohos-aarch64" else []
        cmake_command = [
            "cmake",
            "-DCMAKE_INSTALL_PREFIX={}_{}".format(install_prefix, target_arch),
            "-DCMAKE_BUILD_TYPE={}".format(mode),
            "-DCOPYGC_FLAG=1",
            "-DDOPRA_FLAG=1",
            "-DOHOS_FLAG={}".format(ohos_flag),
            "-DOHOS_ROOT={}".format(args.target_toolchain or ""),
            "-DOHOS_PUBLIC_SDK={}".format(os.path.abspath(args.ohos_public_sdk) if args.ohos_public_sdk else ""),
            "-DANDROID_FLAG=0",
            "-DIOS_FLAG=0",
            "-DIOS_SIMULATOR_FLAG=0",
            "-DEULER_FLAG=0",
            "-DRUNTIME_TRACE_FLAG=0",
            "-DASAN_FLAG=0",
            "-DHWASAN_FLAG={}".format(hwasan_flag),
            "-DSANITIZER_SUPPORT={}".format(args.sanitizer_support),
            "-DCOV=0",
            "-DDUMPADDRESS_FLAG=0",
            "-DCJ_SDK_VERSION={}".format(version),
            "-DDISABLE_VERSION_CHECK=1",
            "-S", ".", "-B", "CMakebuild"
        ] + ptrauth_flags
        build_target(cmake_command, args)

    elif target_args in ["android-aarch64", "android23-aarch64", "android26-aarch64", "android31-aarch64", "android-x86_64", "android23-arm"]:
        if args.target_toolchain == None:
            print("Please configure android toolchain, for example '/root/workspace/android_dep_files/'")
            sys.exit(1)
        android_api_level = 31
        if target_args == "android-aarch64" or target_args == "android23-aarch64" or target_args == "android26-aarch64" or target_args == "android31-aarch64":
            android_flag = "1"
            target_arch = "aarch64"
        elif target_args == "android-x86_64":
            android_flag = "2"
            target_arch = "x86_64"
        elif target_args == "android23-arm":
            android_flag = "3"
            target_arch = "arm"
        android_api_level = re.match(r"android(\d{2})?", target_args).group(1)
        cmake_command = [
            "cmake",
            "-DCMAKE_INSTALL_PREFIX={}_{}".format(install_prefix, target_arch),
            "-DCMAKE_BUILD_TYPE={}".format(mode),
            "-DCOPYGC_FLAG=1",
            "-DDOPRA_FLAG=1",
            "-DOHOS_FLAG=0",
            "-DANDROID_FLAG={}".format(android_flag),
            "-DANDROID_ROOT={}".format(args.target_toolchain),
            "-DIOS_FLAG=0",
            "-DIOS_SIMULATOR_FLAG=0",
            "-DEULER_FLAG=0",
            "-DRUNTIME_TRACE_FLAG=0",
            "-DASAN_FLAG=0",
            "-DHWASAN_FLAG={}".format(hwasan_flag),
            "-DSANITIZER_SUPPORT=0",
            "-DCOV=0",
            "-DDUMPADDRESS_FLAG=0",
            "-DCJ_SDK_VERSION={}".format(version),
            "-DDISABLE_VERSION_CHECK=1",
            "-DCMAKE_ANDROID_API={}".format(android_api_level if android_api_level else "31"),
            "-DCMAKE_SHARED_LINKER_FLAGS={}".format(shared_linker_flags),
            "-DCMAKE_EXE_LINKER_FLAGS={}".format(exe_linker_flags),
            "-S", ".", "-B", "CMakebuild"
        ]
        build_target(cmake_command, args)

    elif target_args in ["ios-aarch64", "ios-simulator-aarch64", "ios-simulator-x86_64"]:
        if args.target_toolchain == None:
            print("Please configure ios toolchain, for example '/root/workspace/ios_dep_files/'")
            sys.exit(1)
        if args.target_sysroot == None:
            print("Please configure ios sysroot, for example from 'xcrun --sdk iphoneos --show-sdk-path'")
            sys.exit(1)
        os.environ["PATH"] = os.path.join(args.target_toolchain, "bin") + ":" + os.environ["PATH"]
        os.environ["SDKROOT"] = os.path.join(args.target_sysroot)
        # ios_flag: 1 means ios-aarch64 real device; 0 means ios simulator or non-ios.
        # ios_simulator_flag: 1 means aarch64 simulator; 2 means x86_64 simulator; 0 means not simulator.
        ios_flag = "1" if target_args == "ios-aarch64" else "0"
        if target_args == "ios-simulator-aarch64":
            target_arch = "aarch64"
            ios_simulator_flag = "1"
        elif target_args == "ios-simulator-x86_64":
            target_arch = "x86_64"
            ios_simulator_flag = "2"
        else:
            ios_simulator_flag = "0"

        cmake_command = [
            "cmake",
            "-DCMAKE_INSTALL_PREFIX={}_{}".format(install_prefix, target_arch),
            "-DCMAKE_BUILD_TYPE={}".format(mode),
            "-DCOPYGC_FLAG=1",
            "-DDOPRA_FLAG=1",
            "-DOHOS_FLAG=0",
            "-DANDROID_FLAG=0",
            "-DIOS_FLAG={}".format(ios_flag),
            "-DIOS_SIMULATOR_FLAG={}".format(ios_simulator_flag),
            "-DEULER_FLAG=0",
            "-DRUNTIME_TRACE_FLAG=0",
            "-DASAN_FLAG=0",
            "-DHWASAN_FLAG={}".format(hwasan_flag),
            "-DSANITIZER_SUPPORT=0",
            "-DCOV=0",
            "-DDUMPADDRESS_FLAG=0",
            "-DCJ_SDK_VERSION={}".format(version),
            "-DDISABLE_VERSION_CHECK=1",
            "-DCMAKE_SYSTEM_NAME=iOS",
            "-DCMAKE_OSX_SYSROOT={}".format(args.target_sysroot),
            "-DCMAKE_OSX_ARCHITECTURES={}".format("arm64" if target_arch == "aarch64" else target_arch),
            "-S", ".", "-B", "CMakebuild"
        ]
        build_target(cmake_command, args)

    else:
        print("Invalid build target, build targets include: native, windows-x86_64, ohos-aarch64, ohos-x86_64, \
               ohos-arm, android-aarch64, android23-aarch64, android26-aarch64, android31-aarch64, android-x86_64, android23-arm, \
               ios-aarch64, ios-simulator-aarch64, ios-simulator-x86_64")
        sys.exit(1)

def build_target(cmake_command, args=None):
    build_jobs = resolve_build_jobs(args.jobs if args else 0)
    env = build_environment(build_jobs)
    # Preserve the interpreter already executing this entry. Do not rediscover
    # a different Python after adding target tools to PATH. CMake caches this
    # absolute host path for configure, build, and repeat publication.
    cmake_command.append("-DPython3_EXECUTABLE:FILEPATH=" + sys.executable.replace("\\", "/"))
    print(f"Runtime build Python: {sys.executable} (host={sys.platform}, pathsep={os.pathsep!r})", flush=True)
    if args and args.gcc_toolchain and args.target == "native":
        cmake_command.append("-DBUILD_GCC_TOOLCHAIN={}".format(args.gcc_toolchain))
    try:
        if "-DWINDOWS_FLAG=1" in cmake_command:
            query = os.path.join(script_path, "CMakebuild", ".cmake", "api", "v1", "query", "client-runtime-exports")
            os.makedirs(query, exist_ok=True)
            with open(os.path.join(query, "codemodel-v2"), "w"):
                pass
        subprocess.run(cmake_command, check=True, env=env)

        build_dir = os.path.join(script_path, "CMakebuild")
        os.makedirs(build_dir, exist_ok=True)
        os.chdir(build_dir)

        subprocess.run(["make", "cangjie-runtime", f"-j{build_jobs}", "VERBOSE=1"], check=True, env=env)

        subprocess.run(["make", "preinstall", f"-j{build_jobs}", "VERBOSE=1"], check=True, env=env)

        print("Build and preinstall completed successfully.")
    except subprocess.CalledProcessError as e:
        print(f"Error during build : {e}")
        sys.exit(1)
    finally:
        os.chdir(script_path)

def do_install(args):
    try:
        build_dir = os.path.join(script_path, "CMakebuild")
        os.makedirs(build_dir, exist_ok=True)
        os.chdir(build_dir)
        macos_flag = 1 if platform.system().lower() == 'darwin' else 0
        if macos_flag:
            os.environ["ZERO_AR_DATE"] = "1"
        if args.prefix == "":
            subprocess.run(["cmake", "-P", "cmake_install.cmake"], check=True)
        else:
            subprocess.run(["cmake", "-DCMAKE_INSTALL_PREFIX={}".format(args.prefix), "-P", "cmake_install.cmake"], check=True)

        print("Install completed successfully.")
    except subprocess.CalledProcessError as e:
        print(f"Error during install : {e}")
        sys.exit(1)
    finally:
        os.chdir(script_path)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="build / install / clean"
    )
    sub = parser.add_subparsers(dest="command", required=True)

    b = sub.add_parser("build", help="compile the project")
    b.set_defaults(func=do_build)
    b.add_argument(
        "--jobs", "-j", type=nonnegative_jobs, default=0,
        help="run N jobs in parallel (0 uses parent budget or effective CPUs)"
    )
    b.add_argument(
        "--target",
        choices=[
            "native",
            "windows-x86_64",
            "ohos-aarch64",
            "ohos-x86_64",
            "ohos-arm",
            "ios-simulator-aarch64",
            "ios-simulator-x86_64",
            "ios-aarch64",
            "android-aarch64",
            "android23-aarch64",
            "android26-aarch64",
            "android31-aarch64",
            "android-x86_64",
            "android23-arm"
        ],
        metavar="TARGET",
        default="native",
        help="Target platform: native, windows-x86_64, ohos-aarch64, ohos-x86_64, ohos-arm, ios-simulator-aarch64, ios-simulator-x86_64, \
              ios-aarch64, android-aarch64, android23-aarch64, android26-aarch64, android31-aarch64, android-x86_64, android23-arm"
    )
    b.add_argument("--testable-internals", action="store_true", help="Enable native runtime internal test hooks.")
    b.add_argument(
        "-t", "--build-type",
        choices=["release", "debug", "relwithdebinfo"],
        default="release",
        help="Build build-type: release, debug, relwithdebinfo (default: release)"
    )
    b.add_argument(
        "-v", "--version",
        default="0.0.1",
        help="Version string, e.g., 0.0.1"
    )
    b.add_argument(
        "--prefix",
        default=os.path.join(script_path, "output", "common"),
        help="Specify the installation directory for the build artifacts."
    )
    b.add_argument(
        "--hwasan",
        action="store_true",
        help="Enable HWASAN"
    )
    b.add_argument(
        "--target-toolchain",
        help="The toolchain required for cross-compilation depends on the specific build target; please specify the appropriate toolchain according to each build-target."
    )
    b.add_argument(
        "--ohos-public-sdk",
        help="Public OpenHarmony Native SDK root containing llvm/ and sysroot/ (CMake OHOS_PUBLIC_SDK)."
    )
    b.add_argument(
        "--target-sysroot",
        help="The sysroot required for cross-compilation depends on the specific build target; please specify the appropriate sysroot according to each build-target."
    )
    b.add_argument(
        "--cjlib-sanitizer-support",
        type=str,
        choices=["asan", "tsan", "hwasan"],
        dest="sanitizer_support",
        help="Enable cangjie runtime sanitizer support."
    )
    b.add_argument(
        "--enable-euler",
        action="store_true",
        dest="euler_flag",
        help="Enable feature on euler os."
    )
    b.add_argument(
        "--gcc-toolchain", dest="gcc_toolchain", help="Specify GCC toolchain for Clang to use"
    )
    b.add_argument(
        "--gc-unit-ohos-host",
        action="store_true",
        dest="gc_unit_ohos_host",
        help="Native x86_64 Linux product with __OHOS__ preprocessor shape for gc_unit (not OHOS ABI)."
    )

    i = sub.add_parser("install", help="install the project")
    i.set_defaults(func=do_install)
    i.add_argument(
        "--prefix",
        default="",
        help="Specify the installation directory for the build artifacts."
    )

    c = sub.add_parser("clean", help="remove build artifacts")
    c.set_defaults(func=do_clean)

    args = parser.parse_args()
    args.func(args)
