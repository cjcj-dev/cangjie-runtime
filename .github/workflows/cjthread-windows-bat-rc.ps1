$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$bat = Join-Path $root "runtime\build\build_cjthread_windows.bat"
if (-not (Test-Path $bat)) { throw "missing $bat" }
$work = Join-Path $env:RUNNER_TEMP "cjthread-bat-rc"
New-Item -ItemType Directory -Force -Path $work | Out-Null
$logs = Join-Path $root "bat-rc-logs"
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$stubSrc = Join-Path $work "stub.c"
@'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <direct.h>
int main(int argc, char **argv) {
    const char *base = argv[0];
    const char *slash = strrchr(base, '\\');
    if (slash) base = slash + 1;
    int is_make = strstr(base, "mingw32-make") != NULL;
    const char *rcs = getenv(is_make ? "WANTED_MAKE_RC" : "WANTED_CMAKE_RC");
    int rc = rcs ? atoi(rcs) : 99;
    char *cwd = _getcwd(NULL, 0);
    const char *log = getenv("RUNLOG");
    if (log) {
        FILE *f = fopen(log, "a");
        if (f) {
            fprintf(f, "%s rc=%d cwd=%s argc=%d", is_make ? "MAKE" : "CMAKE", rc, cwd ? cwd : "?", argc);
            for (int i = 1; i < argc; i++) fprintf(f, " arg%d=%s", i, argv[i]);
            fprintf(f, "\n");
            fclose(f);
        }
    }
    free(cwd);
    return rc;
}
'@ | Set-Content -Encoding ascii $stubSrc
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vcvars = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find "VC\Auxiliary\Build\vcvars64.bat" | Select-Object -First 1
if (-not $vcvars) { throw "vcvars64.bat not found" }
$stubDir = Join-Path $work "stubs"
New-Item -ItemType Directory -Force -Path $stubDir | Out-Null
cmd /c "call `"$vcvars`" && cl /nologo /O2 /Fe:`"$stubDir\cmake.exe`" `"$stubSrc`""
if ($LASTEXITCODE -ne 0) { throw "cl cmake.exe rc=$LASTEXITCODE" }
Copy-Item "$stubDir\cmake.exe" "$stubDir\mingw32-make.exe"
$start = Join-Path $work "start"
New-Item -ItemType Directory -Force -Path $start | Out-Null
cmd /c "subst X: /d" | Out-Null
cmd /c "subst X: `"$start`""
if ($LASTEXITCODE -ne 0) { throw "subst rc=$LASTEXITCODE" }
$driver = Join-Path $work "driver.cmd"
@'
@echo off
set "PATH=%STUBDIR%;%PATH%"
cd /d X:\
set "CJTHREAD_BUILD_PATH=%BUILDDIR%"
echo CASE=%CASE%>>%RUNLOG%
cmd /c call "%PRODUCTBAT%" %BATARGS%
set "CMDC_RC=%ERRORLEVEL%"
>>%RUNLOG% echo CMDC_RC=%CMDC_RC%
cd /d X:\
call "%PRODUCTBAT%" %BATARGS%
set "CALL_RC=%ERRORLEVEL%"
>>%RUNLOG% echo CALL_RC=%CALL_RC%
>>%RUNLOG% echo AFTER=%CD%
exit /b 0
'@ | Set-Content -Encoding ascii $driver
$cases = @(
    @{ name = "positive"; args = ""; bat = (Join-Path $work "positive.bat"); cmake = 0; make = 0; want = 17; tools = "none" },
    @{ name = "success_win"; args = "-p windows_x86_64 Debug STATIC"; bat = $bat; cmake = 0; make = 0; want = 0; tools = "both"; target = "win" },
    @{ name = "cmake_fail_win"; args = "-p windows_x86_64 Debug STATIC"; bat = $bat; cmake = 17; make = 0; want = 17; tools = "cmake"; target = "win" },
    @{ name = "make_fail_win"; args = "-p windows_x86_64 Debug STATIC"; bat = $bat; cmake = 0; make = 23; want = 23; tools = "both"; target = "win" },
    @{ name = "cmake_fail_ohos_aarch64"; args = "-p ohos_aarch64_cangjie Debug STATIC"; bat = $bat; cmake = 19; make = 0; want = 19; tools = "cmake"; target = "ohos" },
    @{ name = "cmake_fail_ohos_x86_64"; args = "-p ohos_x86_64_cangjie Debug STATIC"; bat = $bat; cmake = 19; make = 0; want = 19; tools = "cmake"; target = "ohos" },
    @{ name = "cmake_fail_ohos_arm"; args = "-p ohos_arm_cangjie Debug STATIC"; bat = $bat; cmake = 19; make = 0; want = 19; tools = "cmake"; target = "ohos" },
    @{ name = "make_fail_ohos_aarch64"; args = "-p ohos_aarch64_cangjie Debug STATIC"; bat = $bat; cmake = 0; make = 29; want = 29; tools = "both"; target = "ohos" },
    @{ name = "nop"; args = "not_a_mode"; bat = $bat; cmake = 0; make = 0; want = 0; tools = "none" }
)
Set-Content -Encoding ascii (Join-Path $work "positive.bat") "@echo off`r`nexit /b 17`r`n"
$fail = 0
foreach ($n in 1, 2, 3) {
    foreach ($c in $cases) {
        $log = Join-Path $logs ("{0}-{1}.log" -f $c.name, $n)
        $build = Join-Path $work ("build-{0}-{1}" -f $c.name, $n)
        New-Item -ItemType Directory -Force -Path $build | Out-Null
        if (Test-Path $log) { Remove-Item $log }
        $env:RUNLOG = $log
        $env:STUBDIR = $stubDir
        $env:BUILDDIR = $build
        $env:PRODUCTBAT = $c.bat
        $env:BATARGS = $c.args
        $env:CASE = "$($c.name) N=$n"
        $env:WANTED_CMAKE_RC = "$($c.cmake)"
        $env:WANTED_MAKE_RC = "$($c.make)"
        cmd /c $driver
        $text = if (Test-Path $log) { Get-Content $log -Raw } else { "" }
        $cmdc = if ($text -match "CMDC_RC=(\d+)") { $Matches[1] } else { "MISSING" }
        $call = if ($text -match "CALL_RC=(\d+)") { $Matches[1] } else { "MISSING" }
        $after = if ($text -match "AFTER=(.+)") { $Matches[1].Trim() } else { "MISSING" }
        $cmakeN = ([regex]::Matches($text, "(?m)^CMAKE ")).Count
        $makeN = ([regex]::Matches($text, "(?m)^MAKE ")).Count
        $cmakeLine = ([regex]::Matches($text, "(?m)^CMAKE .+$") | Select-Object -Last 1).Value
        $bad = @()
        if ($cmdc -ne "$($c.want)") { $bad += "cmdc=$cmdc want=$($c.want)" }
        if ($call -ne "$($c.want)") { $bad += "call=$call want=$($c.want)" }
        if ($c.tools -eq "none" -and ($cmakeN -ne 0 -or $makeN -ne 0)) { $bad += "tools ran" }
        if ($c.tools -eq "cmake" -and ($cmakeN -lt 1 -or $makeN -ne 0)) { $bad += "cmake=$cmakeN make=$makeN" }
        if ($c.tools -eq "both" -and ($cmakeN -lt 1 -or $makeN -lt 1)) { $bad += "cmake=$cmakeN make=$makeN" }
        if ($c.tools -ne "none" -and $after -ne "X:\") { $bad += "after=$after" }
        if ($c.target -eq "win" -and $cmakeLine -and $cmakeLine -notmatch "CMAKE_C_COMPILER_TARGET") { $bad += "windows line missing target" }
        if ($c.target -eq "ohos" -and $cmakeLine -and $cmakeLine -match "CMAKE_C_COMPILER_TARGET") { $bad += "ohos took windows line" }
        $mark = if ($bad.Count -eq 0) { "PASS" } else { "FAIL"; $fail++ }
        $line = "$mark $($c.name) N=$n cmdc=$cmdc call=$call cmake=$cmakeN make=$makeN after=$after $($bad -join ',')"
        Write-Output $line
        Add-Content -Path (Join-Path $logs "summary.txt") -Value $line
    }
}
cmd /c "subst X: /d" | Out-Null
if ($fail -ne 0) { throw "BAT_RC_FAILS=$fail" }
Write-Output "BAT_RC_FAILS=0"
