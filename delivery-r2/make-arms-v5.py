from pathlib import Path
import subprocess, shutil, difflib, json
wt=Path('/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_606_implement_r5674249495')
scratch=Path('/root/cj_build/agent_scratch/sym_cangjie_runtime_606_implement_r5674249495/v5')
head=subprocess.check_output(['git','-C',str(wt),'rev-parse','HEAD'],text=True).strip()
common=Path(subprocess.check_output(['git','-C',str(wt),'rev-parse','--git-common-dir'],text=True).strip()).resolve()
paths=['runtime/src/Heap/z/zLiveMap.inline.hpp','runtime/src/Heap/z/zGeneration.cpp','runtime/src/Heap/z/zMark.cpp']
original={p:(wt/p).read_text() for p in paths}
changes={}
s=original[paths[0]];a=s.index('bool RegionBitmap::MarkBits(');b=s.index('namespace MapleRuntime',a)
part=s[a:b].replace('return false;','return __cut_true;').replace('return true;','return false;').replace('return __cut_true;','return true;')
changes['cut-strong']={paths[0]:s[:a]+part+s[b:]}
changes['cut-final']={paths[0]:s.replace('        return incLive;','        return !incLive;')}
s=original[paths[1]];a=s.index('void GenerationCycle::StartOldMark(');b=s.index('// ZGenerationOld::relocate_start',a);part=s[a:b].replace('    collector.flip_old_mark_start();\n','').replace('    PublishPhase(GC_PHASE_ENUM);','    collector.flip_old_mark_start();\n    PublishPhase(GC_PHASE_ENUM);')
changes['cut-old-order']={paths[1]:s[:a]+part+s[b:]}
a=s.index('YoungCollectionStats GenerationCycle::StartYoungMark');b=s.index('// ZGenerationOld::mark_start',a);part=s[a:b].replace('        Heap::GetHeap().GetRememberedSet().FlipForMinor();\n','').replace('    PublishPhase(GC_PHASE_ENUM);','    Heap::GetHeap().GetRememberedSet().FlipForMinor();\n    PublishPhase(GC_PHASE_ENUM);')
changes['cut-young-order']={paths[1]:s[:a]+part+s[b:]}
s=original[paths[2]]
changes['cut-consumer']={paths[2]:s.replace('            bool wasMarked = collector->MarkEntryObject(object, entry, &ctx.Cache());','            bool wasMarked = (collector->MarkEntryObject(object, entry, &ctx.Cache()), false);')}
changes['cut-producer']={paths[2]:s.replace('            workStack.push_back(MarkStackEntry::Claimed(object, firstLive, true, finalizable));','            // Cut arm: omit real root-to-worker publication.')}
s=original[paths[1]]
a=s.index('void GenerationCycle::StartOldMark(');b=s.index('// ZGenerationOld::relocate_start',a)
part=s[a:b].replace('    space.GetRegionManager().RetireSharedPages(kPageAgeRangeOld);', '    space.GetRegionManager().RetireSharedPages(kPageAgeRangeOld);\n    space.AssembleGarbageCandidates();')
changes['cut-mixed-selection']={paths[1]:s[:a]+part+s[b:]}
(wt/'delivery-r2/v5').mkdir(exist_ok=True)
for arm,files in changes.items():
 root=scratch/arm
 if root.exists(): raise RuntimeError('refusing to overwrite '+str(root))
 root.mkdir(parents=True)
 shutil.copytree(wt/'runtime',root/'runtime')
 shutil.copy2(wt/'AGENTS.md',root/'AGENTS.md')
 subprocess.run(['git','-C',str(root),'init','-q'],check=True)
 (root/'.git/objects/info/alternates').write_text(str(common/'objects')+'\n')
 subprocess.run(['git','-C',str(root),'update-ref','HEAD',head],check=True)
 subprocess.run(['git','-C',str(root),'read-tree',head],check=True)
 patch=''
 for name,value in files.items():
  if value==original[name]:raise RuntimeError('unchanged cut '+arm)
  (root/name).write_text(value)
  patch+=''.join(difflib.unified_diff(original[name].splitlines(True),value.splitlines(True),fromfile='a/'+name,tofile='b/'+name))
 (wt/'delivery-r2/v5'/f'{arm}.diff').write_text(patch)
print(json.dumps({'head':head,'arms':list(changes),'scratch':str(scratch)},indent=2))
