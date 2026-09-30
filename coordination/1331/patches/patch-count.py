import pathlib, sys
t = pathlib.Path(sys.argv[1]) / 'runtime/src/Heap/z'
h = t / 'zValue.hpp'; s = t / 'zValue.inline.hpp'
hs = h.read_text(); ss = s.read_text()
# Restore the construction-time count cache the candidate removed.
hs = hs.replace('    uint32_t            _value_id;', '    uint32_t            _value_id;\n    const uint32_t _count;')
hs = hs.replace('    uint32_t                  _value_id;', '    uint32_t                  _value_id;\n    const uint32_t _count;')
ss = ss.replace('_value_id(0) {}', '_value_id(0), _count(S::count()) {}')
ss = ss.replace('_value_id(other._value_id) {}', '_value_id(other._value_id), _count(S::count()) {}')
ss = ss.replace('_value_id < S::count()', '_value_id < _count')
h.write_text(hs); s.write_text(ss)
