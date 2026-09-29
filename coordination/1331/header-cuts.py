from pathlib import Path
import shutil
r=Path('/root/sym_cangjie_runtime_1331_implement_r5895529148')
for arm in ('cut-count','cut-allocation'):
    dst=r/arm/'runtime'
    shutil.copytree(r/'default/runtime',dst,dirs_exist_ok=True)
    h=dst/'src/Heap/z/zValue.hpp'
    p=dst/'src/Heap/z/zValue.inline.hpp'
    hs=h.read_text();s=p.read_text()
    if arm=='cut-count':
        hs=hs.replace('    uint32_t            _value_id;', '    uint32_t            _value_id;\n    const uint32_t _count;').replace('    uint32_t                  _value_id;', '    uint32_t                  _value_id;\n    const uint32_t _count;')
        s=s.replace('_value_id(0) {}', '_value_id(0), _count(S::count()) {}').replace('_value_id(other._value_id) {}', '_value_id(other._value_id), _count(S::count()) {}').replace('_value_id < S::count()', '_value_id < _count')
    else:
        hs=hs.replace('    static uintptr_t _end;', '    static uintptr_t _end;\n    static uint32_t _block_count;')
        s=s.replace('template <typename T> uintptr_t ZValueStorage<T>::_top = 0;', 'template <typename T> uintptr_t ZValueStorage<T>::_top = 0;\ntemplate <typename T> uint32_t ZValueStorage<T>::_block_count = 0;')
        s=s.replace('if (_top < _end)', 'if (_top < _end && S::count() == _block_count)').replace('    _end = _top + Offset;', '    _end = _top + Offset;\n    _block_count = S::count();')
    h.write_text(hs);p.write_text(s)
