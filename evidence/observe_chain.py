import gdb, json, os, struct
# Reuse the earlier raw-memory page-map reader; no inferior function calls.
exec(open(os.environ['B19_HIST_INPUT']).read().split('class CreatedBP')[0])
tracked=[]; identities=set(); forwarded={}; slots={}; events=[]; failures=[]
counts=dict(created=0,registered=0,visit=0,discover=0,pending=0,enqueue_call=0,enqueue_splice=0,execute=0,young_mark=0,old_mark=0)
out=os.environ['GDB_COUNT_OUT']
def stack():
 f=gdb.newest_frame(); rows=[]
 while f and len(rows)<18:
  sal=f.find_sal(); rows.append(dict(function=f.name(),file=sal.symtab.filename if sal.symtab else None,line=sal.line));f=f.older()
 return rows
def arg(name,reg='rsi'):
 try:return int(gdb.parse_and_eval(name))
 except:return int(gdb.parse_and_eval('$'+reg))
def record(kind,obj,**kw):
 if obj in identities:
  events.append(dict(kind=kind,obj=hex(obj),snapshot=inspect_obj(obj),**kw))
def save():
 with open(out,'w') as f:json.dump(dict(counts=counts,tracked=[hex(x) for x in tracked],forwarded={hex(k):hex(v) for k,v in forwarded.items()},events=events,errors=failures),f,indent=2)
class BP(gdb.Breakpoint):
 def __init__(self,spec,fn):
  super().__init__(spec,internal=True);self.fn=fn;self.silent=True
 def stop(self):
  try:
   if '::' in self.location:
    frame=gdb.newest_frame()
    while frame and self.location not in (frame.name() or ''):frame=frame.older()
    if frame:frame.select()
   self.fn()
  except Exception as e:failures.append(dict(spec=self.location,error=str(e)))
  return False
class Finish(gdb.FinishBreakpoint):
 def __init__(self,fn):super().__init__(gdb.selected_frame(),internal=True);self.fn=fn;self.silent=True
 def stop(self):
  try:self.fn(self.return_value)
  except Exception as e:failures.append(dict(finish_error=str(e)))
  return False
def created():
 counts['created']+=1;a=arg('this','rdi')
 if counts['created']==1:
  pid=gdb.selected_inferior().pid
  open(out+'.maps','w').write(open('/proc/%d/maps'%pid).read())
 if len(tracked)<3:tracked.append(a);identities.add(a);record('created',a)
def batch():
 processor=int(gdb.parse_and_eval('this'));objlist=gdb.parse_and_eval('objs');head=objlist['_M_impl']['_M_node'];end=int(head.address);node=int(head['_M_next']);rows=[]
 while node!=end:
  slot=node+16;word=ru64(slot);a=word&((1<<48)-1);counts['registered']+=1
  if a in identities:rows.append((a,slot,word));record('registration_before_transfer',a,slot=hex(slot),word=hex(word),stack=stack())
  node=ru64(node)
 def done(rv):
  fp=gdb.parse_and_eval('(MapleRuntime::FinalizerProcessor*)%d'%processor)
  head=fp['finalizers']['_M_impl']['_M_node'];end=int(head.address);node=int(head['_M_next'])
  while node!=end:
   slot=node+16;word=ru64(slot);a=word&((1<<48)-1);record('registration_after_transfer',a,slot=hex(slot),word=hex(word));slots[a]=slot;node=ru64(node)
 Finish(done)
def relocate():
 a=arg('obj')
 if a not in identities:return
 record('relocate_before',a,stack=stack())
 def done(rv):
  to=int(rv)
  if to:forwarded[a]=to;identities.add(to);record('relocate_after',to,from_address=hex(a))
 Finish(done)
def mark(kind):
 a=arg('object');counts[kind]=counts.get(kind,0)+1
 if a in identities:
  record(kind+'_before',a,stack=stack());Finish(lambda rv:record(kind+'_after',a))
def phase(name):
 for original in tracked:
  current=original; seen=set()
  while current in forwarded and current not in seen:seen.add(current);current=forwarded[current]
  slot=slots.get(original,0)
  record(name,current,original=hex(original),slot=hex(slot),word=hex(ru64(slot)) if slot else None)
def predicate():
 counts['visit']+=1
 a=arg('finalizerObj')
 try:slot=int(gdb.parse_and_eval('&ref'));word=ru64(slot)
 except:
  candidates=[s for original,s in slots.items() if original==a or forwarded.get(original)==a]
  slot=candidates[0] if len(candidates)==1 else 0
  word=ru64(slot) if slot else 0
 record('discover_predicate',a,slot=hex(slot),word=hex(word),stack=stack())
def discover():
 counts['discover']+=1;a=arg('reference');record('discover',a)
def pending():
 frames=stack()
 if not any('ProcessReferencesImpl' in (r['function'] or '') for r in frames):return
 node=gdb.parse_and_eval('node')
 a=int(node['reference'])
 counts['pending']+=1
 record('pending',a,stack=frames)
def enqueue():
 counts['enqueue_call']+=1;a=arg('candidate');record('enqueue_before',a)
def enqueued():
 counts['enqueue_splice']+=1
 f=gdb.newest_frame()
 while f and 'EnqueueFinalizableReference' not in (f.name() or ''):f=f.older()
 if f:f.select()
 record('enqueue_splice',arg('candidate'))
def execute():
 frames=stack()
 if any('ProcessFinalizableList' in (r['function'] or '') for r in frames):
  counts['execute']+=1;record('execute',arg('finalizeObjAddr','rdi'),stack=frames)
gdb.execute('set pagination off');gdb.execute('set confirm off');gdb.execute('set breakpoint pending on')
BP('MapleRuntime::BaseObject::OnFinalizerCreated',created)
BP('MapleRuntime::FinalizerProcessor::RegisterFinalizers',batch)
BP('MapleRuntime::WCollector::RelocateObjectInner',relocate)
BP('MapleRuntime::WCollector::MarkOldObjectIfActive',lambda:mark('old_mark'))
# This optional symbol only exists in the candidate; baseline retains PushYoungObject.
if os.environ.get('B19_CANDIDATE')=='1':BP('MapleRuntime::WCollector::MarkYoungRootObject',lambda:mark('young_mark'))
else:BP('MapleRuntime::WCollector::PushYoungObject',lambda:mark('generic_push'))
BP('MapleRuntime::WCollector::StartYoungMarkWork',lambda:phase('young_mark_start'))
BP('MapleRuntime::WCollector::StartOldMarkWork',lambda:phase('old_mark_start'))
BP('MapleRuntime::WCollector::FixMinorRootSlots',lambda:phase('young_remap_roots'))
BP('zGeneration.cpp:'+os.environ['B19_DISCOVERY_LINE'],predicate)
BP('MapleRuntime::ReferenceProcessor::DiscoverReference',discover)
BP('MapleRuntime::ReferenceProcessor::Push',pending)
BP('MapleRuntime::FinalizerProcessor::EnqueueFinalizableReference',enqueue)
BP('FinalizerProcessor.cpp:'+os.environ['B19_ENQUEUE_LINE'],enqueued)
BP('ExecuteCangjieStub',execute)
gdb.events.exited.connect(lambda e:(counts.update(inferior_exit=getattr(e,'exit_code',None)),save()))
class Dump(gdb.Command):
 def __init__(self):super().__init__('b19-dump',gdb.COMMAND_USER)
 def invoke(self,a,t):save()
Dump()
