from pathlib import Path
import re

def calls(s, pattern, transform):
 pos=0; out=''
 while True:
  m=re.search(pattern,s[pos:])
  if not m: return out+s[pos:]
  start=pos+m.start(); argstart=pos+m.end(); i=argstart; depth=1; args=[]; a=i
  while depth:
   c=s[i]
   if c=='(': depth+=1
   elif c==')': depth-=1
   elif c==',' and depth==1: args.append(s[a:i].strip()); a=i+1
   i+=1
  args.append(s[a:i-1].strip()); out+=s[pos:start]+transform(m,args); pos=i

for p in Path('runtime/src').rglob('*'):
 if p.suffix not in ('.cpp','.h','.hpp'): continue
 s=p.read_text()
 if p.name=='zAddress.inline.hpp': continue
 current={'current_remapped':'ZPointerRemapped','current_remapped_young_mask':'ZPointerRemappedYoungMask','current_remapped_old_mask':'ZPointerRemappedOldMask','current_marked_young':'ZPointerMarkedYoung','current_marked_old':'ZPointerMarkedOld','current_finalizable':'ZPointerFinalizable','current_remembered':'ZPointerRemembered'}
 s=calls(s,r'ColourPredicates::(\w+)\(',lambda m,a:current[m[1]] if m[1] in current else ('(!is_null_any(to_zpointer('+a[0]+')))' if m[1]=='has_address' else 'ZPointer::'+m[1]+'(to_zpointer('+a[0]+'))'))
 if p.name!='ColourEncoding.h':
  s=calls(s,r'MakeStoreGoodSlotWord\(', lambda m,a:'raw(ZAddress::store_good(to_zaddress('+a[0]+')))')
 s=s.replace('ColorAddressMarkYoungGood(', 'ZAddress::mark_young_good(')
 p.write_text(s)

p=Path('runtime/src/Common/ColourEncoding.h');s=p.read_text();a=s.index('// Single source of truth');s=s[:a]+'''
inline bool IsPlainNonNullSlotWord(uintptr_t value)
{
    return value != 0 && (value & ZPointerAllMetadataMask) == 0;
}
inline SlotWordVerdict ClassifySlotWord(uintptr_t value)
{
    if (value == 0) { return SlotWordVerdict::kNull; }
    return is_valid(static_cast<zpointer>(value)) ? SlotWordVerdict::kColoured : SlotWordVerdict::kIllegal;
}
} // namespace MapleRuntime
#endif // MRT_COLOUR_ENCODING_H
'''; s=s.replace('__attribute__((visibility("hidden"))) constexpr bool IsAddressLayoutSealValid','__attribute__((visibility("hidden"))) inline bool IsAddressLayoutSealValid');s=s.replace('heap.end <= kPointerAddressLimit && typeInfo.end <= kPointerAddressLimit','heap.start >= ZAddressHeapBase && heap.end <= ZAddressHeapBase + ZAddressOffsetMax &&\n        typeInfo.end <= kPointerAddressLimit');p.write_text(s)

p=Path('runtime/src/Heap/z/zBarrier.cpp'); s=p.read_text();a=s.index('// Preserve mark/remember');b=s.index('namespace {',a);s=s[:a]+s[b:];s=calls(s,r'ColourLoadGood\(',lambda m,a:'ZAddress::load_good(from_object('+a[0]+'), '+a[1]+')');s=s.replace('    const uintptr_t loadBad = ::g_cjLoadBadMask;\n','').replace('    const uintptr_t markBad = ::g_cjMarkBadMask;\n','').replace('    const uintptr_t storeBad = ::g_cjStoreBadMask;\n','');p.write_text(s)

p=Path('runtime/src/ObjectModel/RefField.h');s=p.read_text();a=s.index('    MAddress GetAddress() const');b=s.index('    ~HeapSlot()',a);s=s[:a]+'''    MAddress GetAddress() const
    {
        return fieldVal >> ZPointer::load_shift_lookup(fieldVal);
    }

'''+s[b:]
a=s.index('#ifdef __arm__',s.index('explicit HeapSlot(zpointer'));b=s.index('    HeapSlot(HeapSlot&&',a);s=s[:a]+'''    HeapSlot(const BaseObject* obj, MAddress colour)
        : fieldVal(raw(ZAddress::color(from_object(obj), colour))) {}

'''+s[b:]
a=s.index('    explicit HeapSlot(const BaseObject* obj)');b=s.index('};\n',a);s=s[:a]+'''    explicit HeapSlot(const BaseObject* obj)
        : fieldVal(raw(ZAddress::store_good(from_object(obj)))) {}
    friend class WCollector;
    using RefFieldValue = MAddress;
    RefFieldValue fieldVal;
'''+s[b+3:]
# The end marker above was the first union member, so replace through the class boundary explicitly.
# Locate obsolete layout residue if still present.
a=s.index('    RefFieldValue fieldVal;')+len('    RefFieldValue fieldVal;');b=s.index('// The sole HeapSlot compare-exchange write',a);s=s[:a]+'\n};\n\n'+s[b:];p.write_text(s)

p=Path('runtime/src/Heap/WCollector/WCollector.h');s=p.read_text();a=s.index('    RefField<> ColourStoreGood(');b=s.index('    // Produce the load-bad',a);s=s[:a]+s[b:];s=calls(s,r'(?<!\w)ColourStoreGood\(',lambda m,a:'RefField<>(ZAddress::store_good('+a[0]+'))');p.write_text(s)
p=Path('runtime/src/Heap/z/zRelocate.cpp');s=p.read_text();s=calls(s,r'(?<!\w)ColourStoreGood\(',lambda m,a:'RefField<>(ZAddress::store_good('+a[0]+'))');p.write_text(s)
# Uncolor adapters are migrated to the appropriate ZPointer variant at each existing call.
for name in ['runtime/src/Inspector/CjHeapData.cpp','runtime/src/Mutator/Mutator.cpp','runtime/src/Heap/Allocator/SlotList.h']:
 p=Path(name);s=p.read_text();s=calls(s,r'uncolor_bits\(',lambda m,a:'ZPointer::uncolor_unsafe('+a[0]+')');p.write_text(s)
