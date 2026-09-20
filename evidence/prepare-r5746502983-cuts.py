from pathlib import Path
import shutil, difflib, json
root=Path.cwd()
scratch=Path('/root/cj_build/agent_scratch/sym_cangjie_runtime_730_implement_r5746502983')
mutations={
 'tlab-producer':('Heap/z/zCollectedHeap.cpp','_heap.alloc_tlab(AlignUp(requestedSize, size_t{8}))','_heap.alloc_tlab(AlignUp(requestedSize, size_t{8}) + 8)'),
 'tlab-consumer':('Heap/z/zThreadLocalAllocBuffer.cpp','    FillTLAB(start, actualSize);','    FillTLAB(start, actualSize - 8);'),
 'small':('Heap/z/zObjectAllocator.cpp','return alloc_object_in_shared_page(shared_small_page_addr(), ZPageType::small, ZPageSizeSmall, size, flags);','return alloc_object_in_shared_page(shared_small_page_addr(), ZPageType::small, ZPageSizeSmall, size + 8, flags);'),
 'medium-producer':('Heap/z/zObjectAllocator.cpp','            fastMedium.set_fast_medium();','            // controlled cut: omit the fast-medium request flag'),
 'medium-consumer':('Heap/z/zHeap.cpp','manager.TakeRegion(num, role, expectPhysicalMem, allowSaferegion, clearPayload, age, flags);','manager.TakeRegion(num, role, expectPhysicalMem, allowSaferegion, clearPayload, age, role == ZPageType::medium ? ZAllocationFlags{} : flags);'),
 'large':('Heap/z/zObjectAllocator.cpp','alloc_page(ZPageType::large, AlignUp(size, ZGranuleSize), flags, clearPayload);','alloc_page(ZPageType::large, AlignUp(size, ZGranuleSize) + ZGranuleSize, flags, clearPayload);'),
 'segment-producer':('ObjectModel/MArray.inline.h','        if (UNLIKELY(useSegmentedClear)) {','        if (false && UNLIKELY(useSegmentedClear)) {'),
 'segment-consumer':('Heap/z/zObjArrayAllocator.cpp','    complete->SetInvisibleObject(false);','    complete->SetInvisibleObject(true);'),
 'stall':('Heap/z/zPageAllocator.cpp','    ScopedEnterSaferegion enterSaferegion(false);\n    const bool satisfied = request.Wait();','    // controlled cut: wait without entering a saferegion\n    const bool satisfied = request.Wait();'),
}
manifest=[]
for name,(rel,before,after) in mutations.items():
    arm=scratch/name
    assert not arm.exists(),arm
    shutil.copytree(root/'runtime', arm/'runtime')
    # Read-only metadata reference supplies the same candidate stamp to the official build entry.
    shutil.copyfile(root/'.git',arm/'.git')
    path='runtime/src/'+rel
    original=(root/path).read_text()
    assert original.count(before)==1,(name,original.count(before))
    changed=original.replace(before,after)
    (arm/path).write_text(changed)
    cut=root/'evidence'/('730-r5746502983-cut-'+name+'.diff')
    cut.write_text(''.join(difflib.unified_diff(original.splitlines(True),changed.splitlines(True),fromfile='a/'+path,tofile='b/'+path)))
    manifest.append({'name':name,'source':str(arm),'cut':str(cut),'path':path,'line':original[:original.index(before)].count('\n')+1})
(root/'evidence/730-r5746502983-arms.json').write_text(json.dumps(manifest,indent=2)+'\n')
