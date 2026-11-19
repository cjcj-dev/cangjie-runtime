// Cangjie runtime for wasm32-wasi (WASI preview1), staged subset.
// Single instance, single thread. The native runtime (HotSpot-derived ZGC,
// stackmaps, CJThread, signals, mmap) is not part of this build.
#ifndef CJ_WASM_RUNTIME_H
#define CJ_WASM_RUNTIME_H
#include <stddef.h>
#include <stdint.h>

// Object model shared with cjcj codegen for 32-bit pointers (IRBuilder.GetPayloadFromObject:
// one pointer slot of header; %ArrayLayout = { %ArrayBase{i64 length}, [0 x T] } follows it):
//   objects: [TypeInfo* (4)][payload...]
//   arrays:  [TypeInfo* (4)][int64 length @4][elements @12...]
// The native runtime's TYPEINFO_PTR_SIZE is likewise sizeof(TypeInfo*).
#define CJW_OBJECT_HEADER_SIZE 4u
#define CJW_ARRAY_LENGTH_OFFSET 4u
#define CJW_ARRAY_HEADER_SIZE 12u

// ---- arena / collector boundary ------------------------------------------
// Every managed allocation goes through cjw_alloc. M0: bump arena on top of
// wasi-libc malloc, never reclaimed. M1 replaces the collector side (mark from
// the shadow root stack + static roots, sweep) without changing callers.
void* cjw_alloc(size_t size, size_t align);
// Collection request; M0 has no collector and reports it explicitly.
void cjw_collect(void);
size_t cjw_arena_bytes(void);

// ---- process arguments (recorded by CJ_MRT_SetCommandLineArgs) -----------
int cjw_argc(void);
const char** cjw_argv(void);

// ---- unsupported features (fail loudly, distinct exit codes) --------------
_Noreturn void cjw_unsupported(const char* feature, int code);
#define CJW_RC_EXCEPTION 134
#define CJW_RC_UNSUPPORTED 135
#define CJW_RC_PKGINIT 136
#define CJW_RC_TYPEQUERY 137   // runtime generic/interface type queries (M1)
#define CJW_RC_THREADS 138     // blocking on a monitor/condition in a single-threaded instance

// TypeInfo layout as emitted by cjcj for 32-bit pointers (CGTypeInfo.cj, %TypeInfo):
// name@0 kind@4 flag@5 fieldNum@6 size(instance|component)@8 gctib@12 uuid@16
// align@20 typeArgNum@21 inheritNum@22 offsets@24 ... fields@36 super|component@40 mTable@44
#define CJW_TI_KIND_OFFSET 4u
#define CJW_TI_SIZE_OFFSET 8u
#define CJW_TI_SUPER_OFFSET 40u
#define CJW_KIND_CLASS (-128)
#define CJW_KIND_INTERFACE (-127)
#endif
