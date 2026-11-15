// Pure log classification; no service, backend, device or subprocess access.
export function classifyExit({stdout, stderr, logcat, adbRc, adbSignal, adbError, mode, remoteRuntime}) {
  const starts = [...stdout.matchAll(/^EXIT_EVENT pid=(\d+) realtime=([\d.]+) name=start result=0$/gm)];
  const pid = starts.length === 1 ? Number(starts[0][1]) : null;
  const rows = logcat.split(/\r?\n/).filter(line => {
    // adb logcat -v epoch: realtime PID TID priority tag : message.
    const match = line.match(/^\s*([\d.]+)\s+(\d+)\s+(\d+)\s+([VDIWEF])\s/);
    return pid !== null && match && Number(match[2]) === pid && Number(match[1]) >= Number(starts[0][2]);
  });
  const internalFatal = /FORTIFY:|Fatal signal\s+\d+|\bSIGABRT\b|\bSIGSEGV\b|Abort message:/.test(stderr + '\n' + rows.join('\n'));
  const marker = name => new RegExp(`^EXIT_EVENT pid=${pid} realtime=[\\d.]+ name=${name} result=0$`, 'm').test(stdout);
  const nativeMarkers = pid !== null && marker('init') && marker('return') && (mode !== 'fini' || marker('fini'));
  const loadedExpected = pid !== null && stdout.includes(`EXIT_RUNTIME pid=${pid} path=${remoteRuntime}`);
  const outerSuccess = adbRc === 0 && adbSignal === null && !adbError;
  const waits = [...stdout.matchAll(/^EXIT_SHELL native_wait_rc=(\d+)$/gm)];
  const nativeWaitRc = waits.length === 1 ? Number(waits[0][1]) : null;
  return {pid, startRealtime: starts[0]?.[2] ?? null, nativeMarkers, loadedExpected, outerSuccess,
    nativeWaitRc, nativeSignal: 'not independently returned by adb; use same-PID fatal records',
    internalFatal, pidLogcat: rows.join('\n') + '\n',
    pass: outerSuccess && nativeWaitRc === 0 && nativeMarkers && loadedExpected && !internalFatal,
    meaning: 'one common-C-API/no-image execution; no full-image fix or mutex-cause qualification'};
}
