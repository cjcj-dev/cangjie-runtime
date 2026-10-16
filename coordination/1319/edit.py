from pathlib import Path
r=Path('runtime/src')
def edit(name, fn):
 p=r/name; p.write_text(fn(p.read_text()))
edit('Heap/z/zThreadLocalData.hpp',lambda s:s.replace('    // Stable owner kind;', '''    // ZGC zThreadLocalData.hpp:101-112: the slot belongs to the active
    // allocation window, not to the mutator's permanent root storage.
    void set_invisible_root(zaddress_unsafe* root)
    {
        assert(invisibleRoot == nullptr);
        invisibleRoot = root;
    }
    void clear_invisible_root()
    {
        assert(invisibleRoot != nullptr);
        invisibleRoot = nullptr;
    }
    zaddress_unsafe* invisible_root() const { return invisibleRoot; }

    // Stable owner kind;'''))
for name in ['Heap/z/zBarrierSet.cpp','Heap/z/zBarrierSet.hpp']:
 edit(name,lambda s:s.replace(', zaddress_unsafe* root)', ')').replace('    data.invisibleRoot = root;\n',''))
edit('Mutator/ThreadLocal.cpp',lambda s:s.replace('nativeData, nullptr, tls, nullptr','nativeData, nullptr, tls'))
edit('Heap/z/zStackWatermark.cpp',lambda s:s.replace('owner.GetGCData().invisibleRoot','owner.GetGCData().invisible_root()'))
edit('Heap/z/zObjArrayAllocator.cpp',lambda s:s.replace('    mutator->PublishInvisibleRoot(array);','    zaddress_unsafe mem = unsafe(from_object(array));\n    mutator->GetGCData().set_invisible_root(&mem);').replace('static_cast<MArray*>(mutator->LoadInvisibleRoot())','static_cast<MArray*>(to_object(safe(mem)))').replace('    MArray* complete = static_cast<MArray*>(mutator->WithdrawInvisibleRoot());','    mutator->GetGCData().clear_invisible_root();\n    MArray* complete = static_cast<MArray*>(to_object(safe(mem)));'))
def cpp(s):
 s=s.replace('gcData, this, nullptr, reinterpret_cast<zaddress_unsafe*>(&rawObject)','gcData, this, nullptr').replace('    StorePlain(rawObject, zaddress::null);\n','')
 s=s.replace('VisitStackRoots(visitor, visitor)','VisitStackRoots(visitor)').replace('const RootVisitor& func, const RootVisitor& invisibleRootVisitor','const RootVisitor& func')
 a=s.index('    const RootVisitor& visitedInvisibleRootVisitor'); b=s.index('    if (!IsManagedContext())',a)
 s=s[:a]+s[b:]
 s=s.replace('        VisitRawObjects(visitedInvisibleRootVisitor);\n','').replace('    VisitRawObjects(visitedInvisibleRootVisitor);\n','')
 a=s.index('void Mutator::VisitRawObjects('); b=s.index('void Mutator::VisitNativeFrameRoots(',a); s=s[:a]+s[b:]
 s=s.replace('derivedPtrVisitor, rootVisitor, young','derivedPtrVisitor, young').replace('const RootVisitor& rawObjectVisitor, bool young','bool young').replace('        VisitRawObjects(rawObjectVisitor);\n','').replace('    VisitRawObjects(rawObjectVisitor);\n','')
 s=s.replace('    // No managed frame means there is no stack map to visit. Side roots are\n    // stored outside the managed stack and still require relocation repair.','    // Native frames have no managed stack map to visit.')
 s=s.replace('const RootVisitor& exceptionRootVisitor, const RootVisitor& rawObjectVisitor,','const RootVisitor& exceptionRootVisitor,').replace('derivedPtrVisitor, rawObjectVisitor, young','derivedPtrVisitor, young')
 return s
edit('Mutator/Mutator.cpp',cpp)
def hdr(s):
 a=s.index('        VisitMutatorRoots(visitor, visitor);'); b=s.index('        VisitExceptionRoots(visitor);',a); s=s[:a]+s[b:]
 s=s.replace('VisitStackRoots(visitor, invisibleRootVisitor)','VisitStackRoots(visitor)').replace('const DerivedPtrVisitor& derivedPtrVisitor, const RootVisitor& exceptionRootVisitor,\n                             const RootVisitor& rawObjectVisitor, bool young = false','const DerivedPtrVisitor& derivedPtrVisitor, const RootVisitor& exceptionRootVisitor,\n                             bool young = false')
 a=s.index('    // A newly allocated large reference array'); b=s.index('    void MutatorLock()',a); s=s[:a]+s[b:]
 s=s.replace('const RootVisitor& func, const RootVisitor& invisibleRootVisitor','const RootVisitor& func').replace('const RootVisitor& rawObjectVisitor, bool young = false','bool young = false').replace('    void VisitRawObjects(const RootVisitor& func);\n','').replace('    ObjectRef rawObject{};\n','')
 return s
edit('Mutator/Mutator.h',hdr)
edit('Heap/z/zHeapIterator.cpp',lambda s:s.replace('}); }, [](ObjectRef&) {});','}); });'))
p=Path('runtime/tests/gc_unit/test_segmented_array_init.cpp');s=p.read_text().replace('MArray* array = static_cast<MArray*>(target->LoadInvisibleRoot());','zaddress_unsafe* slot = target->GetGCData().invisible_root();\n                MArray* array = slot == nullptr ? nullptr : static_cast<MArray*>(to_object(safe(*slot)));').replace('mutator->LoadInvisibleRoot() == nullptr','mutator->GetGCData().invisible_root() == nullptr');p.write_text(s)
p=Path('runtime/tests/gc_unit/test_native_root_current.cpp');s=p.read_text().replace('    RootSlot* historicalSlot = nullptr;','    RootSlot* historicalSlot = nullptr;\n    zaddress_unsafe invisibleMem = unsafe(from_object(from));').replace('            thread->PublishInvisibleRoot(from);\n            thread->VisitMutatorRoots([&](ObjectRef& root) {\n                if (raw(root.LoadPlain()) == reinterpret_cast<uintptr_t>(from)) historicalSlot = &root;\n            });','            thread->GetGCData().set_invisible_root(&invisibleMem);\n            historicalSlot = reinterpret_cast<RootSlot*>(&invisibleMem);').replace('(void)thread->WithdrawInvisibleRoot()','thread->GetGCData().clear_invisible_root()').replace('    RootSlot* slot;','    RootSlot* slot;\n    zaddress_unsafe invisibleMem = unsafe(from_object(from));').replace('thread->PublishInvisibleRoot(from)','thread->GetGCData().set_invisible_root(&invisibleMem)').replace('thread->GetGCData().invisibleRoot','thread->GetGCData().invisible_root()').replace('thread->WithdrawInvisibleRoot()','thread->GetGCData().clear_invisible_root()');p.write_text(s)
