// wasm32 view of the Cangjie object model and TypeInfo/GCTib metadata, shared
// by the runtime entry points and the collector. Offsets are those emitted by
// cjcj (CGTypeInfo.cj, %TypeInfo) and the Cangjie LLVM for 32-bit targets.
#ifndef CJ_WASM_OBJECT_MODEL_H
#define CJ_WASM_OBJECT_MODEL_H
#include <stdint.h>
#include "CjWasmRuntime.h"

#define CJW_TI_FLAG_OFFSET 5u
#define CJW_TI_GCTIB_OFFSET 12u
#define CJW_FLAG_HAS_REF_FIELD_BIT 0x1u   // runtime/src/ObjectModel/Flags.h:58

// TypeKind (runtime/src/ObjectModel/Flags.h:17-50); every negative kind is a reference.
#define CJW_KIND_RAWARRAY (-126)
#define CJW_KIND_GENERIC_TI (-1)
#define CJW_KIND_GENERIC_CUSTOM (-2)
#define CJW_KIND_VARRAY 20
#define CJW_KIND_TUPLE 21
#define CJW_KIND_STRUCT 22
#define CJW_KIND_ENUM 23

// GCTib on 32-bit targets is always a pointer to { u32 nBytes; u8 bits[nBytes]; }
// (Cangjie LLVM lib/IR/CJStructTypeGCInfo.cpp:152-212: the inline 64-bit word form is
// only used when !isArch32Bit()). Bit i (LSB first within each byte) marks the
// reference slot at byte offset 4*i of the traced region: the payload after the
// TypeInfo pointer for objects, each element for struct arrays.
typedef struct {
    uint32_t nBytes;
    uint8_t bits[];
} CjwGCTib;
#define CJW_GCTIB_SLOT_BYTES 4u

static inline const uint8_t* cjw_obj_ti(const void* obj) { return *(const uint8_t* const*)obj; }
static inline int8_t cjw_ti_kind(const uint8_t* ti) { return *(const int8_t*)(ti + CJW_TI_KIND_OFFSET); }
static inline uint8_t cjw_ti_flag(const uint8_t* ti) { return *(ti + CJW_TI_FLAG_OFFSET); }
static inline uint32_t cjw_ti_size(const uint8_t* ti) { return *(const uint32_t*)(ti + CJW_TI_SIZE_OFFSET); }
static inline const CjwGCTib* cjw_ti_gctib(const uint8_t* ti) { return *(const CjwGCTib* const*)(ti + CJW_TI_GCTIB_OFFSET); }
// super for classes, component for RawArray/VArray TypeInfos.
static inline const uint8_t* cjw_ti_super(const uint8_t* ti) { return *(const uint8_t* const*)(ti + CJW_TI_SUPER_OFFSET); }
static inline int cjw_kind_is_ref(int8_t kind) { return kind < 0; }
static inline int cjw_kind_is_struct(int8_t kind)
{
    return kind == CJW_KIND_STRUCT || kind == CJW_KIND_TUPLE || kind == CJW_KIND_ENUM;
}
#endif
