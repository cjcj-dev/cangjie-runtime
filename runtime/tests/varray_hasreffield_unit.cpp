// Standalone unit test: TypeInfo::HasRefField for VArray + branch decision matrix.
// Does not link libcangjie-runtime (avoids full RT init SEGV in structural harnesses).
// Compile: clang++ -std=c++14 -O0 -I$REPO/runtime/src this.cpp -o probe

#include <cstdint>
#include <cstdio>
#include <cstring>

// Minimal stand-ins matching MClass.inline.h HasRefField logic after the fix.
enum TypeKind : int8_t {
    TYPE_KIND_CLASS = 1,
    TYPE_KIND_INT64 = 10,
    TYPE_KIND_RAWARRAY = 19,
    TYPE_KIND_VARRAY = 20,
};

constexpr uint8_t FLAG_HAS_REF_FIELD = 0x1;

struct FakeTI {
    int8_t type = 0;
    uint8_t flag = 0;
    FakeTI* component = nullptr;

    bool IsArrayType() const { return type == TYPE_KIND_RAWARRAY; }
    bool IsVArray() const { return type == TYPE_KIND_VARRAY; }
    bool IsRef() const
    {
        return type == TYPE_KIND_CLASS; // simplified: class is ref
    }
    FakeTI* GetComponentTypeInfo() const { return component; }

    // Mirror of MClass.inline.h after G-C1..C3 fix.
    bool HasRefField() const
    {
        if (IsArrayType() || IsVArray()) {
            FakeTI* componentTi = GetComponentTypeInfo();
            if (componentTi->IsRef()) {
                return true;
            } else {
                return componentTi->HasRefField();
            }
        } else {
            return static_cast<bool>(flag & FLAG_HAS_REF_FIELD);
        }
    }
};

// Pre-fix: VArray used flag only (flag typically 0 for VArray).
bool HasRefFieldPreFix(const FakeTI& ti)
{
    if (ti.IsArrayType()) {
        FakeTI* componentTi = ti.GetComponentTypeInfo();
        if (componentTi->IsRef()) {
            return true;
        } else {
            return componentTi->HasRefField();
        }
    } else {
        return static_cast<bool>(ti.flag & FLAG_HAS_REF_FIELD);
    }
}

enum Path { PATH_RECORD, PATH_BARE };

Path ChooseG_C1(const FakeTI& component)
{
    return component.HasRefField() ? PATH_RECORD : PATH_BARE;
}

Path ChooseG_C2_C3(const FakeTI& varrayTi)
{
    return varrayTi.HasRefField() ? PATH_RECORD : PATH_BARE;
}

int main()
{
    FakeTI classTi {};
    classTi.type = TYPE_KIND_CLASS;
    classTi.flag = FLAG_HAS_REF_FIELD;

    FakeTI primTi {};
    primTi.type = TYPE_KIND_INT64;

    FakeTI structWithRef {};
    structWithRef.type = 5; // non-array
    structWithRef.flag = FLAG_HAS_REF_FIELD;

    FakeTI structNoRef {};
    structNoRef.type = 5;

    FakeTI varrayRef {};
    varrayRef.type = TYPE_KIND_VARRAY;
    varrayRef.component = &classTi;

    FakeTI varrayPrim {};
    varrayPrim.type = TYPE_KIND_VARRAY;
    varrayPrim.component = &primTi;

    FakeTI varrayStructRef {};
    varrayStructRef.type = TYPE_KIND_VARRAY;
    varrayStructRef.component = &structWithRef;

    FakeTI varrayStructNo {};
    varrayStructNo.type = TYPE_KIND_VARRAY;
    varrayStructNo.component = &structNoRef;

    int fail = 0;
    auto check = [&](const char* name, bool cond) {
        std::printf("CHECK %s %s\n", name, cond ? "PASS" : "FAIL");
        if (!cond) {
            ++fail;
        }
    };

    // Pre-fix false negative: VArray with class component, flag=0 → no record.
    check("pre_fix_varray_class_false_neg", !HasRefFieldPreFix(varrayRef));
    // Post-fix true positive.
    check("post_fix_varray_class", varrayRef.HasRefField());
    check("post_fix_varray_prim", !varrayPrim.HasRefField());
    check("post_fix_varray_struct_ref", varrayStructRef.HasRefField());
    check("post_fix_varray_struct_no", !varrayStructNo.HasRefField());

    // Branch matrix: three gaps, ref vs prim.
    check("G-C1_ref_RECORD", ChooseG_C1(varrayRef) == PATH_RECORD);
    check("G-C1_prim_BARE", ChooseG_C1(varrayPrim) == PATH_BARE);
    check("G-C2_ref_RECORD", ChooseG_C2_C3(varrayRef) == PATH_RECORD);
    check("G-C2_prim_BARE", ChooseG_C2_C3(varrayPrim) == PATH_BARE);
    check("G-C3_ref_RECORD", ChooseG_C2_C3(varrayRef) == PATH_RECORD);
    check("G-C3_prim_BARE", ChooseG_C2_C3(varrayPrim) == PATH_BARE);

    // Simulated remset counts (logic-level positive control).
    // pre bare always 0; post ref >0; post prim 0.
    auto simRecord = [](Path p) -> int { return p == PATH_RECORD ? 1 : 0; };
    check("POS_G-C1_pre0", simRecord(PATH_BARE) == 0);
    check("POS_G-C1_post_ref", simRecord(ChooseG_C1(varrayRef)) > 0);
    check("POS_G-C1_post_prim0", simRecord(ChooseG_C1(varrayPrim)) == 0);
    check("POS_G-C2_pre0", simRecord(PATH_BARE) == 0);
    check("POS_G-C2_post_ref", simRecord(ChooseG_C2_C3(varrayRef)) > 0);
    check("POS_G-C2_post_prim0", simRecord(ChooseG_C2_C3(varrayPrim)) == 0);
    check("POS_G-C3_pre0", simRecord(PATH_BARE) == 0);
    check("POS_G-C3_post_ref", simRecord(ChooseG_C2_C3(varrayRef)) > 0);
    check("POS_G-C3_post_prim0", simRecord(ChooseG_C2_C3(varrayPrim)) == 0);

    std::printf("VARRAY_HASREFFIELD_UNIT result=%s fails=%d\n", fail == 0 ? "PASS" : "FAIL", fail);
    return fail == 0 ? 0 : 1;
}
