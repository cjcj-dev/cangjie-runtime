# Reuses the #1391 machine-entry/return lifecycle observer. No product hooks.
import gdb, struct, json, os, re
failures=[]
records={}
generations={}
carriers={}
joined=set()
tls_completed=set()
teardown_seen=False
bootstrap_tid=0
held_tid=0
held_thread=0
joiner_thread=0
hold_target=os.environ.get("SHUTDOWN1459_HOLD_TARGET", "")
published=0
selected=gdb.execute("show environment GC_UNIT_OTHER_VM_CHILD",to_string=True).split(" = ",1)[1].strip().replace(".", "_")
blocked_mode="UnreturnedNativeCallRetainsStorage" in selected
blocked_result_seen=False
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
            tls_completed.add(gdb.selected_thread().ptid[1])
        return False
class DestructorEntry(gdb.Breakpoint):
    def __init__(self,name,field):
        super().__init__("*'"+name+"'",internal=True); self.field=field
    def stop(self):
        data=int(gdb.parse_and_eval("(unsigned long)&((MapleRuntime::"+self.field[0]+"*)$rdi)->"+self.field[1]))
        destructors[gdb.selected_thread().global_num]=data
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
            return True
        return False
class DestructorRet(gdb.Breakpoint):
    def __init__(self,address): super().__init__("*"+address,internal=True)
    def stop(self):
        data=destructors.pop(gdb.selected_thread().global_num,None)
        if data is not None:
            destructor_result(data,"product_destructor_ret")
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
class JoinReturned(gdb.Breakpoint):
    def __init__(self,address,handle,number):
        super().__init__("*"+hex(address),internal=True,temporary=True)
        self.thread=number
        self.handle=handle
    def stop(self):
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
        if blocked_mode:
            check(False,"unreturned_native_storage_retained")
            print("EVENT BLOCKED_TEARDOWN_RED",flush=True)
            return True
        for handle,tid in carriers.items():
            owners=[r for r in records.values() if r["tid"]==tid]
            check(tid in joined,"carrier_join_before_teardown",tid=tid)
            for r in owners:
                check(r["destroyed"] and (not r["attached"] or r["detached"]),"carrier_owner_paired_before_teardown",tid=tid,generation=r["generation"],owner=hex(r["owner"]))
            check(not owners or tid in tls_completed,"carrier_tls_before_teardown",tid=tid,owners=len(owners))
        if held_tid and held_tid not in tls_completed:
            print("EVENT TEARDOWN_WITH_HELD tid=%d" % held_tid,flush=True)
            return True
        return False
CarrierEntry("*StartCJRuntime",bootstrap=True)
CarrierEntry("*CJ_ThreadEntry")
JoinEntry("*pthread_join",internal=True)
Teardown("*'MapleRuntime::CangjieRuntime::FiniAndDelete()'",internal=True)
class FiniReturned(gdb.Breakpoint):
    def stop(self):
        global blocked_result_seen
        if blocked_mode:
            blocked_result_seen=True
            tid=int(gdb.parse_and_eval("*(long*)&shutdown1459BlockedTid"))
            sample=open("/proc/%d/task/%d/syscall"%(gdb.selected_inferior().pid,tid)).read()
            runtime=int(gdb.parse_and_eval("MapleRuntime::Runtime::runtime"))
            owners=[r for r in records.values() if r["tid"]==tid]
            check(int(gdb.parse_and_eval("$rax"))!=0 and runtime!=0,"unreturned_native_storage_retained",tid=tid,runtime=hex(runtime))
            check(sample.startswith("0 ") and tid not in joined,"unreturned_native_not_joined",tid=tid,syscall=sample.strip())
            check(bool(owners) and all(not r["destroyed"] for r in owners),"unreturned_native_not_destroyed",tid=tid,owners=len(owners))
            for carrier_tid in carriers.values():
                if carrier_tid!=tid:
                    check(carrier_tid in joined,"completed_native_joined_with_blocker",tid=carrier_tid)
        return False
for address in re.findall(r"(0x[0-9a-f]+)[^\n]*\sret[q]?\s*(?:\n|$)",gdb.execute("disassemble FiniCJRuntime",to_string=True)):
    FiniReturned("*"+address,internal=True)
class ConsumerReturn(gdb.Breakpoint):
    def stop(self):
        caller=gdb.newest_frame().older()
        check(False,"native_exit_context_restored",caller=caller.name() if caller else None)
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
    check((blocked_result_seen if blocked_mode else teardown_seen) and bool(carriers),"shutdown_observer_executed",carriers=len(carriers),teardown=teardown_seen,blocked_result=blocked_result_seen)
    check(bool(tls_completed),"actual_tls_return_executed",n=len(tls_completed))
    check(getattr(event,"exit_code",None)==0,"inferior_exit",rc=getattr(event,"exit_code",None))
    print("LIFECYCLE_SUMMARY "+json.dumps(dict(counts=counts,failures=failures,records=len(records),target_records=sum(r["target"] for r in records.values()),destructor_results=selected_destructor_results)))
gdb.events.exited.connect(exited)

print("EVENT controller ready",flush=True)
