// Standalone unit for SUSPECT-02: IsSameRootPackage walked past NUL when two
// names were equal and had no '.' / ':' separator. Product site:
// ObjectModel/MClass.cpp IsSameRootPackage.
//
// Compile:
//   clang++ -std=c++14 -O0 -fsanitize=address runtime/tests/objm_same_root_package_unit.cpp -o /tmp/objm_root

#include <cstdio>
#include <cstring>

namespace {

bool PreFixIsSameRoot(const char* name1, const char* name2, bool* walkedPastNul)
{
    *walkedPastNul = false;
    unsigned pos = 0;
    char ch = name1[pos];
    while (ch == name2[pos]) {
        if (ch == '.' || ch == ':') {
            return true;
        }
        if (ch == '\0') {
            *walkedPastNul = true;
            return false;
        }
        ++pos;
        ch = name1[pos];
        if ((ch == ':' && name2[pos] == '.') || (ch == '.' && name2[pos] == ':')) {
            return true;
        }
    }
    return false;
}

bool PostFixIsSameRoot(const char* name1, const char* name2)
{
    unsigned pos = 0;
    char ch = name1[pos];
    while (ch != '\0' && ch == name2[pos]) {
        if (ch == '.' || ch == ':') {
            return true;
        }
        ++pos;
        ch = name1[pos];
        if ((ch == ':' && name2[pos] == '.') || (ch == '.' && name2[pos] == ':')) {
            return true;
        }
    }
    return false;
}

} // namespace

int main()
{
    int fail = 0;
    auto check = [&](const char* name, bool cond) {
        std::printf("CHECK %s %s\n", name, cond ? "PASS" : "FAIL");
        if (!cond) {
            ++fail;
        }
    };

    bool walked = false;
    check("pre_equal_A_walks_past_nul", !PreFixIsSameRoot("A", "A", &walked) && walked);
    check("post_equal_A_false", !PostFixIsSameRoot("A", "A"));
    check("post_pkg_dot_true", PostFixIsSameRoot("pkg.A", "pkg.B"));
    check("post_pkg_colon_true", PostFixIsSameRoot("pkg:A", "pkg:B"));
    check("post_mixed_sep_true", PostFixIsSameRoot("pkg.A", "pkg:B"));
    check("post_different_false", !PostFixIsSameRoot("A", "B"));
    check("post_prefix_false", !PostFixIsSameRoot("ab", "ac"));

    std::printf("OBJM_SAME_ROOT_PACKAGE result=%s fails=%d\n", fail == 0 ? "PASS" : "FAIL", fail);
    return fail == 0 ? 0 : 1;
}
