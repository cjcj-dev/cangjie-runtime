from pathlib import Path
import re
root=Path('runtime/src'); ref=Path('/root/cj_build/reference/jdk/src/hotspot/share/gc/z')
old_inline=(root/'Heap/z/zAddress.inline.hpp').read_text()
# Preserve infrastructure conversions separately from the ZGC address functions.
helpers=old_inline[old_inline.index('// to_object:'):old_inline.index('// null checks')]
rename={
 'REMAP_COLOUR_MASK':'ZPointerRemappedMask','REMAP_COLOUR_SHIFT':'ZPointerRemappedShift','REMAP_COLOUR_BITS':'ZPointerRemappedBits',
 'MARKED_YOUNG_0':'ZPointerMarkedYoung0','MARKED_YOUNG_1':'ZPointerMarkedYoung1','MARKED_OLD_0':'ZPointerMarkedOld0','MARKED_OLD_1':'ZPointerMarkedOld1',
 'REMEMBERED_0':'ZPointerRemembered0','REMEMBERED_1':'ZPointerRemembered1','REMEMBERED_MASK':'ZPointerRememberedMask',
 'FINALIZABLE_0':'ZPointerFinalizable0','FINALIZABLE_1':'ZPointerFinalizable1','FINALIZABLE_MASK':'ZPointerFinalizableMask',
 'MARKED_YOUNG_MASK':'ZPointerMarkedYoungMask','MARKED_OLD_MASK':'ZPointerMarkedOldMask','STORE_METADATA_MASK':'ZPointerStoreMetadataMask',
 'ZPointerLoadGoodMask':'g_cjLoadGoodMask','ZPointerLoadBadMask':'g_cjLoadBadMask','ZPointerMarkGoodMask':'g_cjMarkGoodMask','ZPointerMarkBadMask':'g_cjMarkBadMask','ZPointerStoreGoodMask':'g_cjStoreGoodMask','ZPointerStoreBadMask':'g_cjStoreBadMask'}
def names(s):
 return re.sub(r'\b('+ '|'.join(rename)+r')\b',lambda m:rename[m[0]],s)
def assertions(s):
 # Convert HotSpot's variadic assert to the standard C++ assertion without altering its condition.
 out=''; pos=0
 for match in list(re.finditer(r'\bassert\(',s)):
  if match.start()<pos: continue
  i=match.end(); start=i; depth=1; comma=None; quote=None
  while depth:
   c=s[i]
   if quote:
    if c=='\\': i+=2; continue
    if c==quote: quote=None
   elif c in "\"'": quote=c
   elif c=='(': depth+=1
   elif c==')': depth-=1
   elif c==',' and depth==1 and comma is None: comma=i
   i+=1
  cond=s[start:comma if comma is not None else i-1]
  out+=s[pos:match.start()]+'assert('+cond+')'; pos=i
 return out+s[pos:]
h=(ref/'zAddress.hpp').read_text(); copyright=h[:h.index('#ifndef')]
constants=h[h.index('// One bit'):h.index('// The bad mask is 64 bit.')]
constants=re.sub(r'extern uintptr_t  ZPointerVector[^;]+;\n','',constants)
constants=constants.replace('LITTLE_ENDIAN_ONLY(0) BIG_ENDIAN_ONLY(4)','0')
classes=h[h.index('class ZOffset'):h.index('#endif // SHARE_GC_Z_ZADDRESS_HPP')].replace(' : public AllStatic','')
# C linkage for the compiler ABI masks; all other state remains in MapleRuntime.
for n in ['LoadGood','LoadBad','MarkGood','MarkBad','StoreGood','StoreBad']:
 constants=re.sub(r'extern uintptr_t\s+ZPointer'+n+r'Mask;\n','',constants)
header=copyright+'#pragma once\n#include <cassert>\n#include <type_traits>\n#include "Base/Types.h"\n#include "Heap/z/zGenerationId.hpp"\nextern "C" {\n'
for n in ['LoadGood','LoadBad','MarkGood','MarkBad','StoreGood','StoreBad']:
 header+='extern unsigned long g_cj'+n+'Mask;\n'
header+='extern size_t g_cjLoadShift;\n}\nnamespace MapleRuntime {\nclass BaseObject;\nextern const bool ZVerifyOops;\n'+constants
header+='''
constexpr uintptr_t ZPointerMarkedYoungMask = ZPointerMarkedYoung0 | ZPointerMarkedYoung1;
constexpr uintptr_t ZPointerMarkedOldMask = ZPointerMarkedOld0 | ZPointerMarkedOld1;
constexpr uintptr_t ZPointerFinalizableMask = ZPointerFinalizable0 | ZPointerFinalizable1;
extern uint32_t* ZPointerStoreGoodMaskLowOrderBitsAddr;
enum class zoffset : Uptr { zero = 0, invalid = UINTPTR_MAX };
enum class zoffset_end : Uptr { invalid = UINTPTR_MAX };
enum class zpointer : Uptr { null = 0 };
enum class zaddress : Uptr { null = 0 };
enum class zaddress_unsafe : Uptr { null = 0 };
'''+classes+'}\n#include "Heap/z/zAddress.inline.hpp"\n'
(root/'Heap/z/zAddress.hpp').write_text(names(header))
s=(ref/'zAddress.inline.hpp').read_text()
# Typed offsets, including the same operator macro and declaration order as ZGC.
off=s[s.index('// Offset Operator Macro'):s.index('// zbacking_offset functions')]
off=off.replace('checked_cast<','static_cast<')
# Validity and typed conversions are separated from oop infrastructure.
valid=s[s.index('#define report_is_valid_failure'):s.index('inline zpointer to_zpointer(oopDesc*')]
valid+='\n'+s[s.index('inline bool is_null(zpointer'):s.index('inline void dereferenceable_test')]
valid+='''inline void dereferenceable_test(zaddress addr) {
  if (ZVerifyOops && addr != zaddress::null) { (void)*reinterpret_cast<volatile uintptr_t*>(static_cast<Uptr>(addr)); }
}
inline zaddress to_zaddress(uintptr_t value) {
  const zaddress addr = static_cast<zaddress>(value);
  assert_is_valid(addr);
  dereferenceable_test(addr);
  return addr;
}
inline zaddress_unsafe to_zaddress_unsafe(uintptr_t value) { return static_cast<zaddress_unsafe>(value); }
inline bool is_null(zaddress_unsafe addr) { return addr == zaddress_unsafe::null; }
'''
valid=valid.replace('DEBUG_ONLY(is_valid(ptr, true /* assert_on_failure */);)', '#ifndef NDEBUG\n  is_valid(ptr, true);\n#endif').replace('DEBUG_ONLY(is_valid(addr, true /* assert_on_failure */);)', '#ifndef NDEBUG\n  is_valid(addr, true);\n#endif')
valid=valid.replace('#ifndef AARCH64','#ifndef __aarch64__')
body=s[s.index('// ZOffset functions'):s.index('#endif // SHARE_GC_Z_ZADDRESS_INLINE_HPP')]
inline=copyright+'#pragma once\n#include "Heap/z/zAddress.hpp"\nnamespace MapleRuntime {\n'
inline+='''constexpr Uptr raw(zpointer p) { return static_cast<Uptr>(p); }
constexpr Uptr raw(zaddress p) { return static_cast<Uptr>(p); }
constexpr Uptr raw(zaddress_unsafe p) { return static_cast<Uptr>(p); }
constexpr Uptr raw(zoffset p) { return static_cast<Uptr>(p); }
inline uintptr_t untype(zaddress_unsafe p) { return raw(p); }
inline bool is_power_of_2(uintptr_t value) { return value != 0 && (value & (value - 1)) == 0; }
inline uintptr_t ZPointer::remap_bits(uintptr_t value) {
#ifdef __aarch64__
  return (value ^ ZPointerRemappedMask) & ZPointerRemappedMask;
#else
  return value & ZPointerRemappedMask;
#endif
}
inline constexpr int ZPointer::load_shift_lookup(uintptr_t value) {
#ifdef __aarch64__
  return 16;
#else
  const size_t index = (value >> ZPointerRemappedShift) & 0xf;
  assert(index == 0 || index == 1 || index == 2 || index == 4 || index == 8);
  return ZPointerLoadShiftTable[index];
#endif
}
'''+off+'\n#undef CREATE_ZOFFSET_OPERATORS\n'+valid+body
inline+='''inline zaddress safe(zaddress_unsafe value) { return to_zaddress(raw(value)); }
'''+helpers+'\n}\n'
inline=names(assertions(inline)).replace('PTR_FORMAT','"%zx"')
(root/'Heap/z/zAddress.inline.hpp').write_text(inline)
# Rename constants at consumers, including tests; expectations are handled separately.
for p in Path('runtime').rglob('*'):
 if p.suffix not in ('.h','.hpp','.cpp','.cc'): continue
 if p.name in ('zAddress.hpp','zAddress.inline.hpp','zAddress.cpp'): continue
 original=p.read_text(); changed=names(original)
 if original!=changed: p.write_text(changed)
