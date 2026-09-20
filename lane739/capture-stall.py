import os, pathlib, signal, subprocess, time
root=pathlib.Path('/root/sym_cangjie_runtime_739_implement_r5747765432-evidence')
out=root/'shutdown-gdb';out.mkdir(exist_ok=True)
lib=pathlib.Path('/root/sym_cangjie_runtime_739_implement_r5747765432-cut-shutdown/testable/build/runtime-staging/lib/x86_64_Release')
elf=root/'final-locked-testable/cj_gc_unit'
case='AllocationStall.ProductShutdownAnswersPendingWaiters'
env=os.environ.copy();env.update(LD_LIBRARY_PATH=str(lib),GC_UNIT_OTHER_VM_CHILD=case,MRT_LOG_LEVEL='e')
(out/'uptime-before.txt').write_text(subprocess.check_output(['uptime'],text=True))
with (out/'stack.log').open('w') as f:
    p=subprocess.Popen(['taskset','-c','16-31','gdb','-batch','-ex','set pagination off','-ex','set confirm off','-ex','run','-ex','thread apply all bt 18','-ex','kill','--args',str(elf),'--gtest_filter='+case],env=env,stdout=f,stderr=subprocess.STDOUT)
    time.sleep(3)
    if p.poll() is None:p.send_signal(signal.SIGINT)
    try:rc=p.wait(timeout=20)
    except subprocess.TimeoutExpired:
        p.kill();rc=p.wait()
(out/'gdb.rc').write_text(str(rc)+'\n')
(out/'uptime-after.txt').write_text(subprocess.check_output(['uptime'],text=True))
(out/'identity.sha256').write_text(subprocess.check_output(['sha256sum',str(elf),str(lib/'libcangjie-runtime.so'),str(lib/'libboundscheck.so')],text=True))
