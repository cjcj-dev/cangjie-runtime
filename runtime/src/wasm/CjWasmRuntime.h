// Cangjie runtime for wasm32-wasi (WASI preview1), staged subset.
// Single instance, single thread. The native runtime (HotSpot-derived ZGC,
// stackmaps, CJThread, signals, mmap) is not part of this build.
#ifndef CJ_WASM_RUNTIME_H
#define CJ_WASM_RUNTIME_H
#include <stddef.h>
#include <stdint.h>

// Object model shared with cjcj codegen (packages/codegen/src/RuntimeLayout.cj,
// 32-bit pointer path): [TypeInfo* | 4 bytes][payload...];
// arrays: [TypeInfo* | 4 bytes][int64 length][elements...].
#define CJW_OBJECT_HEADER_SIZE 8u
#define CJW_ARRAY_LENGTH_OFFSET 8u
#define CJW_ARRAY_HEADER_SIZE 16u

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
#endif
