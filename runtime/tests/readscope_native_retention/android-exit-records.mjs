// Pure log classification; no service, backend, device or subprocess access.
function epochNanoseconds(value) {
  const match = typeof value === 'string' && value.match(/^(\d+)(?:\.(\d{1,9}))?$/);
  return match ? BigInt(match[1]) * 1000000000n + BigInt((match[2] ?? '').padEnd(9, '0')) : null;
}
export function classifyExit({stdout, stderr, logcat, adbRc, adbSignal, adbError, mode, remoteRuntime,
  fullImage = false, crash, captures}) {
  const recordTextComplete = [stdout, stderr, logcat, crash].every(value => typeof value === 'string');
  stdout = typeof stdout === 'string' ? stdout : '';
  stderr = typeof stderr === 'string' ? stderr : '';
  logcat = typeof logcat === 'string' ? logcat : '';
  crash = typeof crash === 'string' ? crash : '';
  const starts = [...stdout.matchAll(/^EXIT_EVENT pid=(\d+) realtime=([\d.]+) name=start result=0$/gm)];
  // Full-image retains the original consumer/return semantics. Its parent shell
  // records the actual background child's PID, a window starting before spawn,
  // and wait($child)'s result, independently of adb's exit status.
  const shells = [...stdout.matchAll(/^EXIT_SHELL child_pid=(\d+) realtime_start=([\d.]+) realtime_end=([\d.]+) native_wait_rc=(\d+)$/gm)];
  const shell = shells.length === 1 ? shells[0] : null;
  const startRealtime = fullImage ? shell?.[2] ?? null : starts.length === 1 ? starts[0][2] : null;
  const endRealtime = fullImage ? shell?.[3] ?? null : null;
  const pid = fullImage ? shell ? Number(shell[1]) : null : starts.length === 1 ? Number(starts[0][1]) : null;
  // logcat may round/truncate realtime to milliseconds; the native marker has
  // nanoseconds. Do not discard a same-PID fatal in the marker's first ms.
  // Decimal parsing also avoids rounding a nanosecond marker up across a
  // millisecond boundary at real (large) epoch values.
  const startNs = epochNanoseconds(startRealtime), endNs = epochNanoseconds(endRealtime);
  const firstMillisecond = startNs === null ? null : startNs / 1000000n;
  const lastMillisecond = endNs === null ? null : endNs / 1000000n;
  const fatal = /FORTIFY:|\bfatal\b|\bSIGABRT\b|\bSIGSEGV\b|Abort message:/i;
  let attributionIncomplete = false;
  const rows = (logcat + '\n' + crash).split(/\r?\n/).filter(line => {
    // adb logcat -v epoch: realtime PID TID priority tag : message.
    const match = line.match(/^\s*([\d.]+)\s+(\d+)\s+(\d+)\s+([VDIWEF])\s/);
    if (match) {
      if (pid === null) {
        attributionIncomplete ||= fatal.test(line) || match[4] === 'F';
        return false;
      }
      const time = epochNanoseconds(match[1]);
      if (Number(match[2]) !== pid) return false;
      if (time === null || firstMillisecond === null) {
        attributionIncomplete ||= fatal.test(line) || match[4] === 'F';
        return false;
      }
      const millisecond = time / 1000000n;
      return millisecond >= firstMillisecond && (lastMillisecond === null || millisecond <= lastMillisecond);
    }
    // Old threadtime records can identify a different PID, but cannot prove a
    // matching PID lies in this epoch window. Do not silently discard a fatal
    // merely because its format/timestamp is missing.
    const legacy = line.match(/^\s*\d\d-\d\d\s+[\d:.]+\s+(\d+)\s+\d+\s+([VDIWEF])\s/);
    if (fatal.test(line) || legacy?.[2] === 'F') {
      attributionIncomplete ||= !legacy || pid === null || Number(legacy[1]) === pid;
    }
    return false;
  });
  // These streams belong to this one target invocation, including its native
  // loader/destructors. A fatal here is already decisive without missing PID
  // markers; incomplete logs must never hide that known failure.
  const internalFatal = fatal.test(stdout + '\n' + stderr + '\n' + rows.join('\n')) ||
    rows.some(line => /^\s*[\d.]+\s+\d+\s+\d+\s+F\s/.test(line));
  const marker = name => new RegExp(`^EXIT_EVENT pid=${pid} realtime=[\\d.]+ name=${name} result=0$`, 'm').test(stdout);
  const nativeMarkers = fullImage ? shell !== null : pid !== null && marker('init') && marker('return') && (mode !== 'fini' || marker('fini'));
  const loadedExpected = fullImage ? true : pid !== null && stdout.includes(`EXIT_RUNTIME pid=${pid} path=${remoteRuntime}`);
  const outerSuccess = adbRc === 0 && adbSignal === null && !adbError;
  const waits = [...stdout.matchAll(/^EXIT_SHELL native_wait_rc=(\d+)$/gm)];
  const nativeWaitRc = fullImage ? shell ? Number(shell[4]) : null : waits.length === 1 ? Number(waits[0][1]) : null;
  const captureOK = item => item?.rc === 0 && item.signal === null && !item.error;
  const captureComplete = !fullImage || (recordTextComplete && captures?.stderr === true && captureOK(captures.logcat) && captureOK(captures.crash));
  const windowValid = !fullImage || (pid > 0 && startNs !== null && startNs > 0n && endNs !== null && endNs >= startNs);
  const pass = outerSuccess && nativeWaitRc === 0 && nativeMarkers && loadedExpected && !internalFatal &&
    (!fullImage || (captureComplete && windowValid && !attributionIncomplete));
  return {pid, startRealtime, endRealtime, nativeMarkers, loadedExpected, outerSuccess,
    nativeWaitRc, nativeSignal: 'not independently returned by adb; use same-PID fatal records',
    internalFatal, captureComplete, windowValid, attributionIncomplete, pidLogcat: rows.join('\n') + '\n', pass,
    outcome: internalFatal || (nativeWaitRc !== null && nativeWaitRc !== 0) ? 'FAIL' : pass ? 'PASS' : 'UNKNOWN',
    meaning: fullImage ? 'one full-image exit observation; no mutex-cause or runtime-fix qualification' :
      'one common-C-API/no-image execution; no full-image fix or mutex-cause qualification'};
}

export function classifyFullImageExit(input) {
  const result = classifyExit({...input, fullImage: true});
  const stdout = typeof input.stdout === 'string' ? input.stdout : '';
  const stderr = typeof input.stderr === 'string' ? input.stderr : '';
  const names = ['elf-native-load', 'elf-constructor-exactly-once', 'elf-public-load',
    'elf-public-native-identity', 'elf-unknown-close-rejected', 'elf-logical-close',
    'elf-external-native-close', 'elf-no-close-fini', 'elf-reopen-same-identity',
    'elf-reopen-no-double-init', 'elf-reopen-close', 'elf-runtime-init', 'elf-published-package'];
  if (input.mode === 'runtime-fini') names.push('elf-runtime-fini', 'elf-code-after-runtime-fini');
  result.targetChecksComplete = ['preinit', 'runtime-fini'].includes(input.mode) &&
    names.every(name => stdout.includes(`TARGET ${name} executed=1 pass=1`));
  result.fixtureCountsComplete = ['INIT', 'FINI'].every(name =>
    ((stdout + '\n' + stderr).match(new RegExp(`^ELF_FIXTURE_${name} count=1$`, 'gm')) ?? []).length === 1);
  result.pass &&= result.targetChecksComplete && result.fixtureCountsComplete;
  if (/TARGET \S+ executed=1 pass=0/.test(stdout)) { result.pass = false; result.outcome = 'FAIL'; }
  else if (!result.pass && result.outcome === 'PASS') result.outcome = 'UNKNOWN';
  return result;
}
