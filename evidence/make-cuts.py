from pathlib import Path
import subprocess
repo=Path.cwd(); head=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip()
root=Path('/root/cj_build/agent_scratch/sym_cangjie_runtime_607_implement_r5738304930/finalcuts')
root.mkdir(parents=True,exist_ok=True)
cuts={
'producer':('runtime/src/Heap/z/zBarrier.cpp', '    StoreBarrier(obj, field, false, weakReferent ? ReferenceStrength::Weak : ReferenceStrength::Strong);','    (void)weakReferent;'),
'consumer':('runtime/src/Heap/z/zGeneration.cpp','        Heap::GetHeap().remembered().scan_and_follow(Heap::GetHeap().young().MarkPtr());','        // P2 cut: omit real remset consumer.'),
'promotion':('runtime/src/Heap/z/zGeneration.cpp','    Heap::page_table().replace(from, to);','    (void)from; (void)to;'),
'partial':('runtime/src/Heap/z/zMark.cpp','ZBarrier::MarkBarrierOnOldOopField(nullptr, field, entry.finalizable());','ZBarrier::MarkBarrierOnOldOopField(nullptr, field, false);'),
'final':('runtime/src/Heap/z/zBarrier.cpp','MarkFinalizableFromOldSlowPath',''),
'strong':('runtime/src/Heap/z/zBarrier.cpp','MarkFromOldSlowPath',''),
'nonmajor':('runtime/src/Heap/z/zBarrier.cpp','    if (young.IsMajorRoots()) {','    if (true) {'),
}
for name,(file,old,new) in cuts.items():
    tree=root/name
    subprocess.run(['git','worktree','add','--detach',str(tree),head],check=True,capture_output=True)
    p=tree/file;s=p.read_text()
    if name in ('final','strong'):
        start=s.index('zaddress ZBarrier::'+old+'(');end=s.index('\n}',start)
        block=s[start:end];assert block.count('return zaddress::null;')==1
        s=s[:start]+block.replace('return zaddress::null;','return address;')+s[end:]
    else:
        assert s.count(old)==1,(name,s.count(old));s=s.replace(old,new)
    p.write_text(s)
    patch=subprocess.check_output(['git','diff'],cwd=tree)
    (repo/'evidence'/f'final-{name}.diff').write_bytes(patch)
    print(name,tree)
