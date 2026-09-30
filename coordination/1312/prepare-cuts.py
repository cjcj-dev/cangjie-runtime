from pathlib import Path
import difflib,json
root=Path(__file__).resolve().parents[2]
out=root/'coordination/1312/cuts';out.mkdir(exist_ok=True)
dedup='runtime/src/Heap/shared/stringdedup/stringDedup.cpp'
barrier='runtime/src/Heap/z/zBarrier.inline.hpp'
spec={
 'keepalive': [(dedup,'auto* found = static_cast<MArray*>(value.resolve());','auto* found = static_cast<MArray*>(value.peek());')],
 'peek':[(dedup,'auto* value = static_cast<MArray*>(values[index].peek());','auto* value = static_cast<MArray*>(values[index].resolve());')],
 'young': [('runtime/src/Heap/z/zRootsIterator.cpp','    weak.apply(&copy);','    // Controlled cut: omit weak storage roots.')],
 'phase':[('runtime/src/Heap/z/zGeneration.cpp','    Heap::GetHeap().old().WeakRootsProcessor()->process_weak_roots();','    // Controlled cut: omit old weak processing.')],
 'report':[('runtime/src/Heap/z/zWeakRootsProcessor.cpp','        rootsWeakColored.report_num_dead();','        // Controlled cut: omit owner notification.')],
 'resize_find':[(dedup,'WeakHandle value = cleanupState->Find(object, hash);','WeakHandle value; // Controlled cut: omit old buckets.')],
 'wait2':[(dedup,'case DeadState::wait2:\n            table.deadState.store(DeadState::wait1, std::memory_order_release);','case DeadState::wait2:\n            table.deadState.store(DeadState::wait2, std::memory_order_release);')],
 'blocked':[(barrier,'inline zaddress ZBarrier::load_barrier_on_phantom_oop_field_preloaded(volatile zpointer* p, zpointer o) {\n  if (ZResurrection::is_blocked()) {','inline zaddress ZBarrier::load_barrier_on_phantom_oop_field_preloaded(volatile zpointer* p, zpointer o) {\n  if (false) {'),(barrier,'inline zaddress ZBarrier::no_keep_alive_load_barrier_on_phantom_oop_field_preloaded(volatile zpointer* p, zpointer o) {\n  if (ZResurrection::is_blocked()) {','inline zaddress ZBarrier::no_keep_alive_load_barrier_on_phantom_oop_field_preloaded(volatile zpointer* p, zpointer o) {\n  if (false) {')],
 'compiler': [('runtime/src/CompilerCalls.cpp','    return StringDedup::Instance().Canonical(arrayInfo, candidate);','    return candidate; // Controlled cut: omit canonical consumer.')],
 'registration': [('runtime/src/Heap/z/zRootsIterator.hpp','std::array<OopStorage::ParState<true>, 4> states;', 'std::array<OopStorage::ParState<true>, 3> states;'),('runtime/src/Heap/z/zRootsIterator.cpp','{SyncWeakOopStorage(), workers},\n              {StringDedup::Instance().WeakStorage(), workers}}}', '{SyncWeakOopStorage(), workers}}}')],
 'strong_registration': [
 ('runtime/src/Heap/z/zRootsIterator.hpp', 'std::array<OopStorage::ParState<true>, 1> states;', 'std::array<OopStorage::ParState<true>, 2> states;'),
 ('runtime/src/Heap/z/zRootsIterator.hpp', 'std::array<OopStorage::ParState<true>, 4> states;', 'std::array<OopStorage::ParState<true>, 3> states;'),
 ('runtime/src/Heap/z/zRootsIterator.cpp', 'StrongRootStorage(), workers}}}', 'StrongRootStorage(), workers}, {StringDedup::Instance().WeakStorage(), workers}}}'),
 ('runtime/src/Heap/z/zRootsIterator.cpp', '{SyncWeakOopStorage(), workers},\n              {StringDedup::Instance().WeakStorage(), workers}}}', '{SyncWeakOopStorage(), workers}}}')],
 'restored':[]}
for name, edits in spec.items():
 texts={}
 for file,old,new in edits:
  before=texts.get(file,(root/file).read_text())
  assert before.count(old)==1,(name,file,old)
  texts[file]=before.replace(old,new)
 diff=''.join(''.join(difflib.unified_diff((root/file).read_text().splitlines(True),after.splitlines(True),fromfile='a/'+file,tofile='b/'+file)) for file,after in texts.items())
 (out/(name+'.diff')).write_text(diff)
(out/'spec.json').write_text(json.dumps(spec,indent=2))
