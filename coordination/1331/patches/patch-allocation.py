import pathlib, sys
t = pathlib.Path(sys.argv[1]) / 'runtime/src/Heap/z'
h = t / 'zValue.hpp'; s = t / 'zValue.inline.hpp'
hs = h.read_text(); ss = s.read_text()
hs = hs.replace('    static uintptr_t _end;', '    static uintptr_t _end;\n    static uint32_t _block_count;')
ss = ss.replace('template <typename T> uintptr_t ZValueStorage<T>::_top = 0;',
                'template <typename T> uintptr_t ZValueStorage<T>::_top = 0;\n'
                'template <typename T> uint32_t ZValueStorage<T>::_block_count = 0;')
ss = ss.replace('if (_top < _end)', 'if (_top < _end && S::count() == _block_count)')
ss = ss.replace('    _end = _top + Offset;', '    _end = _top + Offset;\n    _block_count = S::count();')
h.write_text(hs); s.write_text(ss)
