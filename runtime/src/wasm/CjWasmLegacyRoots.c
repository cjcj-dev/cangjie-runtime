// Legacy shadow frames for the wasm32-wasi Cangjie runtime. See CjWasmHeap.h.
//
// Code that pushes these frames was compiled with proof-based root publication,
// which does not cover every live reference. This archive member is linked only
// when such code references the frame hooks; its constructor then records that
// the program's roots are not complete, so the collector stays off.
#include "CjWasmHeap.h"

void cjw_root_frame_push(CjwRootFrame* frame) { cjw_root_frame_push_complete(frame); }
void cjw_root_frame_pop(CjwRootFrame* frame) { cjw_root_frame_pop_complete(frame); }

__attribute__((constructor)) static void NoteLegacyFrames(void) { cjw_roots_note_legacy_frames(); }
