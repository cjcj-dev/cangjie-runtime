#ifndef MRT_FAKE_RTTI_SUPPORT_HPP
#define MRT_FAKE_RTTI_SUPPORT_HPP

#include <cassert>
#include <climits>
#include <cstdint>

namespace MapleRuntime {
template<typename BaseT, typename TagType>
class FakeRttiSupport {
public:
    explicit FakeRttiSupport(TagType concrete_tag)
        : _tag_set(tag_bit(concrete_tag)), _concrete_tag(concrete_tag) {}

    TagType concrete_tag() const { return _concrete_tag; }

private:
    uintptr_t _tag_set;
    TagType _concrete_tag;

    static uintptr_t tag_bit(TagType tag)
    {
        return uintptr_t{1} << validate_tag(tag);
    }

    static TagType validate_tag(TagType tag)
    {
        assert(static_cast<intptr_t>(tag) >= 0);
        assert(static_cast<uintptr_t>(tag) < sizeof(uintptr_t) * CHAR_BIT);
        return tag;
    }
};
}
#endif
