# Reuses the #1391 machine-entry/return lifecycle observer. No product hooks.
import gdb, struct, json, os, re
failures=[]
records={}
generations={}
carriers={}
joined=set()
tls_completed=set()
native_destructor_data=set()
teardown_seen=False
bootstrap_tid=0
held_tid=0
held_thread=0
joiner_thread=0
joining={}
semaphore_carriers={}
gate_thread=0
gate_used=False
shutdown_started=False
hold_target=os.environ.get("SHUTDOWN1459_HOLD_TARGET", "")
boundary_mode=os.environ.get("SHUTDOWN1459_BOUNDARY", "")
boundary_qualified=False
published=0
selected=gdb.execute("show environment GC_UNIT_OTHER_VM_CHILD",to_string=True).split(" = ",1)[1].strip().replace(".", "_")
blocked_mode="UnreturnedNativeCallRetainsStorage" in selected
blocked_result_seen=False
fini_thread=0
rdlock_entered=False
cut_skipped=False
late_stack_ok=False
late_parked=False
running_samples=[]
last_signal=None
def record_signal(event):
    global last_signal
    if isinstance(event,gdb.SignalEvent):
        thread=event.inferior_thread
        last_signal=(event.stop_signal,thread.ptid[1] if thread is not None else None)
gdb.events.stop.connect(record_signal)
counts={"create":0,"attach":0,"detach":0,"destroy":0}
def check(condition, label, **values):
    print("LIFECYCLE_TARGET " + json.dumps(dict(label=label,ok=bool(condition),**values),sort_keys=True))
    if not condition: failures.append(label)
def ptr(address):
    return struct.unpack("<Q",gdb.selected_inferior().read_memory(address,8))[0]
def slot():
    return int(gdb.parse_and_eval("MapleRuntime::BarrierSet::_barrier_set"))
def buffer(data): return ptr(data+40)
def stack():
    result=[]; f=gdb.newest_frame()
    while f and len(result)<9:
        sal=f.find_sal()
        result.append(dict(function=f.name(),pc=hex(f.pc()),line=sal.line))
        f=f.older()
    return result
pending={}
destructors={}
caller_returns=set()
selected_destructor_results=0
def destructor_result(data, endpoint):
    global selected_destructor_results
    value=buffer(data)
    target=data in records and records[data]["target"]
    print("LIFECYCLE_DESTRUCTOR_RESULT "+json.dumps(dict(data=hex(data),buffer=hex(value),endpoint=endpoint,target=target)))
    if target:
        selected_destructor_results+=1
        check(value==0,"destructor_resource_null",data=hex(data),buffer=hex(value),endpoint=endpoint)

class Return:
    def __init__(self,event,data,owner,trace):
        self.event,self.data,self.owner,self.trace=event,data,owner,trace
    def stop(self):
        event,data=self.event,self.data
        b=buffer(data)
        print("LIFECYCLE_RETURN "+json.dumps(dict(event=event,data=hex(data),buffer=hex(b),stack=self.trace)))
        if event=="create":
            check(b!=0,"create_resource",data=hex(data),buffer=hex(b))
            check(data not in records or records[data]["destroyed"],"create_unique",data=hex(data))
            generations[data]=generations.get(data,0)+1
            records[data]=dict(tid=gdb.selected_thread().ptid[1],generation=generations[data],owner=self.owner,buffer=b,destroyed=False,attached=False,detached=False,stack=self.trace,target=any(selected in (f["function"] or "") for f in self.trace) and not any("InitializeBarrierRuntime" in (f["function"] or "") for f in self.trace))
        elif event=="attach":
            check(data in records and not records[data]["destroyed"],"attach_created",data=hex(data))
            # Early bootstrap attaches legitimately return before masks exist.
            if data in records and ptr(data+24)!=0: records[data]["attached"]=True
        elif event=="detach":
            check(data in records and records[data]["attached"],"detach_attached",data=hex(data))
            if data in records: records[data]["detached"]=True
        elif event=="destroy":
            check(data in records and not records[data]["destroyed"],"destroy_paired",data=hex(data))
            check(b==0,"destroy_resource_null",data=hex(data),buffer=hex(b))
            if data in records:
                check(not records[data]["attached"] or records[data]["detached"],"detach_before_destroy",data=hex(data))
                records[data]["destroyed"]=True
            d=destructors.get(gdb.selected_thread().global_num)
            if d is not None: destructor_result(d,"product_destroy_ret")
        return False
class Entry(gdb.Breakpoint):
    def __init__(self,name,event):
        super().__init__("*'"+name+"'",internal=True)
        self.event=event
    def stop(self):
        global published
        if self.event=="publish":
            published=int(gdb.parse_and_eval("$rdi"))
            print("LIFECYCLE_PUBLISH argument="+hex(published)+" stack="+json.dumps(stack()))
        else:
            owner=int(gdb.parse_and_eval("$rdi")); data=int(gdb.parse_and_eval("$rsi"))
            counts[self.event]+=1
            check(owner==published==slot(),"registered_owner",event=self.event,owner=hex(owner),published=hex(published),slot=hex(slot()),data=hex(data))
            trace=stack()
            key=(gdb.selected_thread().global_num,self.event)
            pending[key]=Return(self.event,data,owner,trace)
            if self.event=="attach" and data in records: records[data]["attached"]=True
            if self.event=="detach" and data in records: records[data]["detached"]=True
        return False
class Ret(gdb.Breakpoint):
    def __init__(self,address,event):
        super().__init__("*"+address,internal=True); self.event=event
    def stop(self):
        key=(gdb.selected_thread().global_num,self.event)
        r=pending.pop(key,None)
        if r: r.stop()
        return False
class OwnerCallerRet(gdb.Breakpoint):
    def __init__(self,address): super().__init__("*"+address,internal=True)
    def stop(self):
        data=destructors.pop(gdb.selected_thread().global_num,None)
        if data is not None:
            destructor_result(data,"owner_caller_return")
            if data in native_destructor_data:
                tls_completed.add(gdb.selected_thread().ptid[1])
        return False
class DestructorEntry(gdb.Breakpoint):
    def __init__(self,name,field):
        super().__init__("*'"+name+"'",internal=True); self.field=field
    def stop(self):
        data=int(gdb.parse_and_eval("(unsigned long)&((MapleRuntime::"+self.field[0]+"*)$rdi)->"+self.field[1]))
        destructors[gdb.selected_thread().global_num]=data
        if self.field[0]=="CleanThreadLocalData": native_destructor_data.add(data)
        return_address=ptr(int(gdb.parse_and_eval("$rsp")))
        if return_address not in caller_returns:
            caller_returns.add(return_address)
            OwnerCallerRet(hex(return_address))
        print("LIFECYCLE_DESTRUCTOR_ENTRY "+json.dumps(dict(data=hex(data),buffer=hex(buffer(data)),stack=stack())))
        global held_tid,held_thread
        tid=gdb.selected_thread().ptid[1]
        target=(hold_target=="bootstrap" and tid==bootstrap_tid) or (hold_target=="worker" and tid!=bootstrap_tid and tid in carriers.values())
        if self.field[0]=="CleanThreadLocalData" and target and not held_tid:
            held_tid=tid
            held_thread=gdb.selected_thread().num
            print("EVENT TLS_HELD tid=%d buffer=%s" % (tid,hex(buffer(data))),flush=True)
            if tid in joining:
                global joiner_thread
                joiner_thread=joining[tid]
                print("EVENT JOIN_WITH_HELD tid=%d"%tid,flush=True)
            return True
        return False
class DestructorRet(gdb.Breakpoint):
    def __init__(self,address): super().__init__("*"+address,internal=True)
    def stop(self):
        data=destructors.pop(gdb.selected_thread().global_num,None)
        if data is not None:
            destructor_result(data,"product_destructor_ret")
            if data in native_destructor_data:
                tls_completed.add(gdb.selected_thread().ptid[1])
        return False
for cls,field in (("Mutator","gcData"),("CleanThreadLocalData","nativeData")):
    name="MapleRuntime::"+cls+"::~"+cls+"()"
    DestructorEntry(name,(cls,field))
    dis=gdb.execute("disassemble '"+name+"'",to_string=True)
    print("LIFECYCLE_DESTRUCTOR_DISASSEMBLY "+dis)
    for address in re.findall(r"(0x[0-9a-f]+)[^\n]*\sret[q]?\s*(?:\n|$)",dis): DestructorRet(address)
Entry("MapleRuntime::BarrierSet::set_barrier_set(MapleRuntime::BarrierSet*)","publish")
for event in counts:
    args="MapleRuntime::ThreadGCData&"
    if event=="attach": args+=", MapleRuntime::Mutator*, MapleRuntime::ThreadLocalData*"
    name="MapleRuntime::ZBarrierSet::on_thread_"+event+"("+args+")"
    Entry(name,event)
    if event in ("create","destroy"):
        dis=gdb.execute("disassemble '"+name+"'",to_string=True)
        print("LIFECYCLE_DISASSEMBLY "+dis)
        for address in re.findall(r"(0x[0-9a-f]+)[^\n]*\sret[q]?\s*(?:\n|$)",dis): Ret(address,event)
class CarrierEntry(gdb.Breakpoint):
    def __init__(self,name,bootstrap=False):
        super().__init__(name,internal=True)
        self.bootstrap=bootstrap
    def stop(self):
        global bootstrap_tid
        # Linux x86-64 glibc pthread_self is the thread descriptor at FS base.
        # The join argument below independently checks this captured identity.
        handle=int(gdb.parse_and_eval("$fs_base"))
        carriers[handle]=gdb.selected_thread().ptid[1]
        if self.bootstrap: bootstrap_tid=carriers[handle]
        print("SHUTDOWN1459_CARRIER_ENTRY "+json.dumps(dict(handle=hex(handle),tid=carriers[handle])))
        return False
class ShutdownBegin(gdb.Breakpoint):
    def stop(self):
        global shutdown_started
        shutdown_started=True
        return False
class WaitEntry(gdb.Breakpoint):
    def stop(self):
        tid=gdb.selected_thread().ptid[1]
        caller=gdb.newest_frame().older()
        if tid in carriers.values() and caller and caller.name() in ("CJ_ThreadEntry", "CJ_ThreadSleep"):
            semaphore_carriers[int(gdb.parse_and_eval("$rdi"))]=tid
        return False
class GateReturned(gdb.Breakpoint):
    def __init__(self,address,number):
        super().__init__("*"+hex(address),internal=True,temporary=True)
        self.thread=number
    def stop(self):
        global gate_thread
        gate_thread=gdb.selected_thread().num
        print("EVENT PRODUCER_GATE",flush=True)
        return True
class PostEntry(gdb.Breakpoint):
    def stop(self):
        global gate_used
        tid=semaphore_carriers.get(int(gdb.parse_and_eval("$rdi")))
        target=(hold_target=="bootstrap" and tid==bootstrap_tid) or (hold_target=="worker" and tid and tid!=bootstrap_tid)
        if shutdown_started and target and not gate_used:
            gate_used=True
            print("EVENT GATE_ARMED tid=%d sem=%s caller=%s"%(tid,hex(int(gdb.parse_and_eval("$rdi"))),gdb.newest_frame().older().name()),flush=True)
            GateReturned(ptr(int(gdb.parse_and_eval("$rsp"))),gdb.selected_thread().num)
        return False
ShutdownBegin("*FiniCJRuntime",internal=True)
WaitEntry("*sem_wait",internal=True)
PostEntry("*sem_post",internal=True)
class JoinReturned(gdb.Breakpoint):
    def __init__(self,address,handle,number):
        super().__init__("*"+hex(address),internal=True,temporary=True)
        self.thread=number
        self.handle=handle
        self.done=False
    def stop(self):
        if self.done: return False
        self.done=True
        rc=int(gdb.parse_and_eval("$rax"))
        tid=carriers[self.handle]
        check(rc==0,"carrier_join_success",tid=tid,rc=rc)
        owners=[r for r in records.values() if r["tid"]==tid]
        check(all(r["destroyed"] for r in owners),"join_after_owner_destroy",tid=tid,owners=len(owners))
        check(not owners or tid in tls_completed,"join_after_tls_return",tid=tid)
        joined.add(tid)
        return False
class JoinEntry(gdb.Breakpoint):
    def stop(self):
        handle=int(gdb.parse_and_eval("$rdi"))
        if handle in carriers:
            joining[carriers[handle]]=gdb.selected_thread().num
            if blocked_mode:
                blocked_tid=int(gdb.parse_and_eval("*(long*)&shutdown1459BlockedTid"))
                if carriers[handle]==blocked_tid:
                    sample=open("/proc/%d/task/%d/syscall"%(gdb.selected_inferior().pid,blocked_tid)).read()
                    check(not sample.startswith("0 "),"unreturned_native_must_not_join",tid=blocked_tid,syscall=sample.strip())
                    if sample.startswith("0 "):
                        print("EVENT BLOCKED_JOIN_RED",flush=True)
                        return True
            global joiner_thread
            if held_tid and carriers[handle]==held_tid:
                joiner_thread=gdb.selected_thread().num
                print("EVENT JOIN_WITH_HELD tid=%d" % held_tid,flush=True)
            JoinReturned(ptr(int(gdb.parse_and_eval("$rsp"))),handle,gdb.selected_thread().num)
        return False
class Teardown(gdb.Breakpoint):
    def stop(self):
        global teardown_seen
        teardown_seen=True
        blocked_tid=0
        if blocked_mode:
            blocked_tid=int(gdb.parse_and_eval("*(long*)&shutdown1459BlockedTid"))
            sample=open("/proc/%d/task/%d/syscall"%(gdb.selected_inferior().pid,blocked_tid)).read()
            check(sample.startswith("0 "),"unreturned_native_still_blocked",tid=blocked_tid,syscall=sample.strip())
            check(blocked_tid not in joined,"unreturned_native_not_joined",tid=blocked_tid)
            owners=[r for r in records.values() if r["tid"]==blocked_tid]
            check(bool(owners) and all(not r["destroyed"] for r in owners),"unreturned_native_not_destroyed",tid=blocked_tid,owners=len(owners))
        if boundary_mode == "producer":
            global boundary_qualified
            owners=[r for r in records.values() if r["tid"]==bootstrap_tid]
            incomplete=bool(owners) and bootstrap_tid not in tls_completed and bootstrap_tid not in joined and all(not r["destroyed"] for r in owners)
            completed=[r for r in records.values() if r["tid"] in tls_completed]
            others_ok=all(r["destroyed"] and (not r["attached"] or r["detached"]) for r in completed)
            boundary_qualified=shutdown_started and bool(carriers) and bool(owners) and others_ok
            check(not incomplete,"bootstrap_completed_before_runtime_delete",tid=bootstrap_tid,owners=len(owners))
            print("BOUNDARY_STATE",boundary_qualified,incomplete,others_ok,flush=True)
            if incomplete:
                print("EVENT PRODUCER_BOUNDARY_RED",flush=True)
                return True
        before_failures=len(failures)
        for handle,tid in carriers.items():
            if blocked_mode and tid==blocked_tid:
                continue
            owners=[r for r in records.values() if r["tid"]==tid]
            check(tid in joined,"carrier_join_before_teardown",tid=tid)
            for r in owners:
                check(r["destroyed"] and (not r["attached"] or r["detached"]),"carrier_owner_paired_before_teardown",tid=tid,generation=r["generation"],owner=hex(r["owner"]))
            check(not owners or tid in tls_completed,"carrier_tls_before_teardown",tid=tid,owners=len(owners))
        if held_tid and held_tid not in tls_completed:
            print("EVENT TEARDOWN_WITH_HELD tid=%d" % held_tid,flush=True)
            return True
        if len(failures)>before_failures:
            print("EVENT TEARDOWN_INCOMPLETE_RED",flush=True)
            return True
        return False
CarrierEntry("*StartCJRuntime",bootstrap=True)
CarrierEntry("*CJ_ThreadEntry")
JoinEntry("*pthread_join",internal=True)
Teardown("*'MapleRuntime::CangjieRuntime::FiniAndDelete()'",internal=True)
def symbol_ready(name):
    symbol=gdb.lookup_global_symbol(name)
    return symbol is not None
def field_bitpos(type_name, field):
    try:
        ty=gdb.lookup_type(type_name)
    except gdb.error:
        check(False,"loaded_type_missing",type=type_name,field=field)
        return None
    for item in ty.fields():
        if item.name==field and item.bitpos is not None:
            return item.bitpos
    check(False,"loaded_field_missing",type=type_name,field=field)
    return None
def mem_u64(address):
    return struct.unpack("<Q",gdb.selected_inferior().read_memory(address,8))[0]
def mem_u8(address):
    return struct.unpack("<B",gdb.selected_inferior().read_memory(address,1))[0]
def flag_at(base, bitpos):
    return (mem_u8(base + bitpos//8) >> (bitpos % 8)) & 1
def ptr_at(base, bitpos):
    if bitpos % 8:
        check(False,"loaded_field_misaligned",bitpos=bitpos)
        return 0
    return mem_u64(base + bitpos//8)
def observe_terminal(mut):
    exited_name="MapleRuntime::VM_Exit::exited()"
    native_name="MapleRuntime::VM_Exit::is_native(MapleRuntime::Mutator const&)"
    state_name="MapleRuntime::SafepointSynchronize::state()"
    if not symbol_ready(exited_name):
        check(False,"exited_symbol_missing")
        exited_vm=0
    else:
        exited_vm=int(gdb.parse_and_eval(exited_name))
    if not symbol_ready(state_name):
        check(False,"safepoint_state_symbol_missing")
        paused=0
    else:
        bit=field_bitpos("MapleRuntime::SafepointState","worldStopped")
        if bit is None:
            paused=0
        else:
            base=int(gdb.parse_and_eval("(unsigned long)&MapleRuntime::SafepointSynchronize::state()"))
            paused=flag_at(base, bit)
    check(exited_vm==1 and paused==1,"logical_terminal_published",exited=exited_vm,paused=paused)
    if mut==0:
        check(False,"unreturned_native_mutator_retained",mut=0)
        return
    term_bit=field_bitpos("MapleRuntime::Mutator","terminatedByVM")
    det_bit=field_bitpos("MapleRuntime::Mutator","gcDetached")
    cj_bit=field_bitpos("MapleRuntime::Mutator","cjthread")
    gc_bit=field_bitpos("MapleRuntime::Mutator","gcData")
    buf_bit=field_bitpos("MapleRuntime::ThreadGCData","storeBarrierBuffer")
    if None in (term_bit,det_bit,cj_bit,gc_bit,buf_bit):
        return
    if gc_bit % 8:
        check(False,"loaded_field_misaligned",field="gcData",bitpos=gc_bit)
        return
    term=flag_at(mut, term_bit)
    detached=flag_at(mut, det_bit)
    carrier=ptr_at(mut, cj_bit)
    buffer_ptr=ptr_at(mut + gc_bit//8, buf_bit)
    if not symbol_ready(native_name):
        check(False,"is_native_symbol_missing")
        native=0
    else:
        native=int(gdb.parse_and_eval("MapleRuntime::VM_Exit::is_native(*(MapleRuntime::Mutator*)%d)"%mut))
    check(term==1 and detached==0 and carrier!=0 and buffer_ptr!=0 and native==1,"unreturned_native_storage_retained",terminated=term,detached=detached,carrier=hex(carrier),buffer=hex(buffer_ptr),native=native)
def release_blocked_read(pid, tid):
    fields=open("/proc/%d/task/%d/syscall"%(pid,tid)).read().split()
    if len(fields)<2 or fields[0]!="0":
        return False
    try:
        read_fd=int(fields[1],0)
    except ValueError:
        return False
    want=os.readlink("/proc/%d/fd/%d"%(pid,read_fd))
    for name in os.listdir("/proc/%d/fd"%pid):
        if name==str(read_fd):
            continue
        path="/proc/%d/fd/%s"%(pid,name)
        try:
            if os.readlink(path)!=want:
                continue
            info=open("/proc/%d/fdinfo/%s"%(pid,name)).read()
        except OSError:
            continue
        writable=False
        for line in info.splitlines():
            if line.startswith("flags:"):
                writable=bool(int(line.split()[1],8)&3)
        if not writable:
            continue
        fd=os.open(path,os.O_WRONLY)
        try:
            os.write(fd,b"x")
        finally:
            os.close(fd)
        return True
    return False
def observe_held_terminal():
    # Inferior function calls are not allowed from Breakpoint.stop callbacks.
    # The controller invokes this only after the Fini breakpoint has stopped.
    mut=int(gdb.parse_and_eval("*(unsigned long*)&shutdown1459BlockedMutator"))
    observe_terminal(mut)
    print("TERMINAL_OBSERVED",len(failures),flush=True)
def blocked_thread():
    tid=int(gdb.parse_and_eval("*(long*)&shutdown1459BlockedTid"))
    for thread in gdb.selected_inferior().threads():
        if thread.ptid[1]==tid:
            return tid,thread
    return tid,None
class RdlockWitness(gdb.Breakpoint):
    def __init__(self):
        super().__init__("pthread_rwlock_rdlock",internal=True)
    def stop(self):
        global rdlock_entered,late_stack_ok
        tid,thread=blocked_thread()
        if gdb.selected_thread().ptid[1]!=tid:
            return False
        names=[(frame["function"] or "") for frame in stack()]
        in_lock=any("pthread_rwlock_rdlock" in name for name in names)
        in_barrier=any("wait_if_vm_exited" in name for name in names)
        from_syscall=any("SyscallExit" in name for name in names)
        top=names[0] if names else ""
        late_stack_ok=in_lock and in_barrier and from_syscall and "wait_if_vm_exited" not in top
        if not late_stack_ok:
            return False
        rdlock_entered=True
        print("EVENT RDLOCK_ENTERED",flush=True)
        return False
class CutRdlockCall(gdb.Breakpoint):
    def __init__(self,call,after):
        super().__init__("*"+hex(call),internal=True)
        self.after=after
        self.done=False
    def stop(self):
        global cut_skipped
        tid,thread=blocked_thread()
        if gdb.selected_thread().ptid[1]!=tid or self.done:
            return False
        self.done=True
        cut_skipped=True
        gdb.execute("set $pc=%d"%self.after)
        gdb.execute("set $eax=0")
        print("EVENT RDLOCK_CUT",flush=True)
        return False
def rdlock_call_site():
    dis=gdb.execute("disassemble 'MapleRuntime::VM_Exit::wait_if_vm_exited()'",to_string=True)
    lines=dis.splitlines()
    for index,line in enumerate(lines):
        if "pthread_rwlock_rdlock" not in line or "call" not in line:
            continue
        here=re.search(r"(0x[0-9a-f]+)",line)
        nxt=re.search(r"(0x[0-9a-f]+)",lines[index+1]) if index+1<len(lines) else None
        if here and nxt:
            return int(here.group(1),16),int(nxt.group(1),16)
    return None
def begin_late(cut):
    pid=gdb.selected_inferior().pid
    tid,thread=blocked_thread()
    if cut:
        site=rdlock_call_site()
        check(site is not None,"late_lock_call_site",site=str(site))
        if site:
            CutRdlockCall(*site)
    else:
        RdlockWitness()
    released=release_blocked_read(pid,tid)
    check(released,"late_return_pipe_released",tid=tid)
    print("LATE_ARMED",int(released),int(bool(cut)),flush=True)
def note_running():
    pid=gdb.selected_inferior().pid
    tid,thread=blocked_thread()
    try:
        wchan=open("/proc/%d/task/%d/wchan"%(pid,tid)).read().strip()
        fields=open("/proc/%d/task/%d/syscall"%(pid,tid)).read().split()
        number=int(fields[0],0) if fields else -1
        stopped=thread is not None and thread.is_stopped()
    except OSError:
        wchan,number,stopped="gone",-1,False
    waiting=(not stopped) and ("futex" in wchan) and number not in (0,-1)
    running_samples.append(waiting)
    print("LATE_RUNNING",int(waiting),wchan,number,int(stopped),flush=True)
def observe_barrier_abort():
    tid,thread=blocked_thread()
    signal_on_late=last_signal==("SIGABRT",tid)
    if thread is not None:
        thread.switch()
    try:
        signo=int(gdb.parse_and_eval("$_siginfo.si_signo"))
    except gdb.error:
        signo=-1
    names=[]
    frame=gdb.newest_frame()
    while frame is not None and len(names)<24:
        names.append(frame.name() or "")
        frame=frame.older()
    on_late=gdb.selected_thread() is not None and gdb.selected_thread().ptid[1]==tid
    in_barrier=any("wait_if_vm_exited" in name for name in names)
    ok=signo==6 and signal_on_late and on_late and in_barrier and cut_skipped and not failures
    check(ok,"permanent_barrier_return_check",signo=signo,on_late=int(on_late),barrier=int(in_barrier),signal_thread=last_signal)
    print("LATE_ABORT_FRAMES",json.dumps(names),flush=True)
    pc=int(gdb.parse_and_eval("$pc"))
    maps=open("/proc/%d/maps"%gdb.selected_inferior().pid).read()
    location=None
    for line in maps.splitlines():
        fields=line.split(None,5)
        begin,end=(int(value,16) for value in fields[0].split("-"))
        if begin<=pc<end:
            location=dict(module=fields[5] if len(fields)>5 else "anonymous",
                          offset=hex(pc-begin+int(fields[2],16)))
            break
    print("LATE_ABORT_PC",json.dumps(dict(pc=hex(pc),location=location)),flush=True)
    print("LATE_ABORT_MAPS\n"+maps,flush=True)
    if ok:
        print("EVENT LATE_BARRIER_ABORT",flush=True)
def finish_late(cut):
    global late_parked
    stable=len(running_samples)>=2 and all(running_samples[-2:])
    if cut:
        check(False,"cut_flag_is_not_a_product_failure",skipped=int(cut_skipped),entered=int(rdlock_entered),stable=int(stable))
        return
    late_parked=rdlock_entered and late_stack_ok and stable
    check(late_parked,"late_return_blocked_in_permanent_lock",entered=int(rdlock_entered),stack=int(late_stack_ok),stable=int(stable),samples=len(running_samples))
    print("EVENT LATE_RETURN_BLOCKED",flush=True)
class FiniReturned(gdb.Breakpoint):
    def stop(self):
        global blocked_result_seen,fini_thread
        if blocked_mode:
            blocked_result_seen=True
            tid=int(gdb.parse_and_eval("*(long*)&shutdown1459BlockedTid"))
            sample=open("/proc/%d/task/%d/syscall"%(gdb.selected_inferior().pid,tid)).read()
            runtime=int(gdb.parse_and_eval("MapleRuntime::Runtime::runtime"))
            owners=[r for r in records.values() if r["tid"]==tid]
            rc=int(gdb.parse_and_eval("$rax"))
            check(rc==0,"logical_fini_ok",tid=tid,rc=rc)
            check(runtime==0,"logical_runtime_facade_deleted",tid=tid,runtime=hex(runtime))
            check(sample.startswith("0 ") and tid not in joined,"unreturned_native_not_joined",tid=tid,syscall=sample.strip())
            check(bool(owners) and all(not r["destroyed"] for r in owners),"unreturned_native_not_destroyed",tid=tid,owners=len(owners))
            for carrier_tid in carriers.values():
                if carrier_tid!=tid:
                    check(carrier_tid in joined,"completed_native_joined_with_blocker",tid=carrier_tid)
            fini_thread=gdb.selected_thread().num
            print("EVENT FINI_HELD",flush=True)
            return True
        return False
for address in re.findall(r"(0x[0-9a-f]+)[^\n]*\sret[q]?\s*(?:\n|$)",gdb.execute("disassemble FiniCJRuntime",to_string=True)):
    FiniReturned("*"+address,internal=True)
class ConsumerReturn(gdb.Breakpoint):
    def stop(self):
        global boundary_qualified
        caller=gdb.newest_frame().older()
        tid=gdb.selected_thread().ptid[1]
        owners=[r for r in records.values() if r["tid"]==tid]
        incomplete=bool(owners) and tid not in tls_completed and all(not r["destroyed"] for r in owners)
        completed=[r for r in records.values() if r["tid"] in tls_completed]
        others_ok=all(r["destroyed"] and (not r["attached"] or r["detached"]) for r in completed)
        boundary_qualified=shutdown_started and tid in carriers.values() and incomplete and others_ok
        check(False,"native_exit_context_restored",tid=tid,owners=len(owners),caller=caller.name() if caller else None)
        print("BOUNDARY_STATE",boundary_qualified,incomplete,others_ok,flush=True)
        print("EVENT CONSUMER_RESTORE_RED",flush=True)
        return True
# In the valid product the restoration transfers control; it never returns to
# ProcessorSchedule/ThreadSleep. A removed context set introduces a real ret.
for address in re.findall(r"(0x[0-9a-f]+)[^\n]*\sret[q]?\s*(?:\n|$)",gdb.execute("disassemble CJ_ProcessorThreadExit",to_string=True)):
    ConsumerReturn("*"+address,internal=True)
def exited(event):
    for data,r in records.items():
        if r["target"] and not blocked_mode:
            check(r["destroyed"],"exit_resource_paired",data=hex(data),buffer=hex(r["buffer"]),create_stack=r["stack"])
        else:
            print("LIFECYCLE_OBSERVED "+json.dumps(dict(data=hex(data),destroyed=r["destroyed"],target=False,create_stack=r["stack"])))
    check(all(counts[x]>0 for x in counts),"all_lifecycle_events",counts=counts)
    check(((blocked_result_seen and late_parked) if blocked_mode else teardown_seen) and bool(carriers),"shutdown_observer_executed",carriers=len(carriers),teardown=teardown_seen,blocked_result=blocked_result_seen,late_return=late_parked)
    check(bool(tls_completed),"actual_tls_return_executed",n=len(tls_completed))
    check(getattr(event,"exit_code",None)==0,"inferior_exit",rc=getattr(event,"exit_code",None))
    print("LIFECYCLE_SUMMARY "+json.dumps(dict(counts=counts,failures=failures,records=len(records),target_records=sum(r["target"] for r in records.values()),destructor_results=selected_destructor_results)))
gdb.events.exited.connect(exited)

print("EVENT controller ready",flush=True)
