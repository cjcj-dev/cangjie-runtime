from pathlib import Path
import subprocess,difflib
repo=Path.cwd(); head=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip()
root=Path('/root/cj_build/agent_scratch/sym_cangjie_runtime_740_implement_r5747608277');root.mkdir(parents=True,exist_ok=True)
barrier='runtime/src/Heap/z/zBarrier.cpp'; inline='runtime/src/Heap/z/zBarrier.inline.hpp'; buffer='runtime/src/Heap/z/zStoreBarrierBuffer.cpp'
cuts={
 'entry':(barrier,'    StoreBarrier(obj, field, false, weakReferent ? ReferenceStrength::Weak : ReferenceStrength::Strong);','    // Controlled cut: omit the real mutator store barrier.'),
 'add':(barrier,'            buffer->add(reinterpret_cast<MAddress>(p), prev);','            // Controlled cut: omit buffered previous-value production.'),
 'remember':(inline,'        page->remember(reinterpret_cast<volatile zpointer*>(address));','        // Controlled cut: omit the page remembered-field publication.'),
 'flush':(buffer,'        ZBarrier::mark_and_remember(reinterpret_cast<volatile zpointer*>(entry.p), addr);','        // Controlled cut: omit the paired entry consumer.'),
}
for name in [*cuts,'restored']:
 tree=root/name
 subprocess.run(['git','worktree','add','--detach',str(tree),head],check=True,stdout=subprocess.DEVNULL)
 if name in cuts:
  path,old,new=cuts[name];p=tree/path;before=p.read_text();assert before.count(old)==1
  p.write_text(before.replace(old,new));diff=''.join(difflib.unified_diff(before.splitlines(True),p.read_text().splitlines(True),fromfile='a/'+path,tofile='b/'+path))
  (repo/f'evidence/740/cut-{name}.diff').write_text(diff)
print(head)
