// Non-executed, compiler-format input for the original metadata consumers.
// Header/table layout is supplied by Cjstart and the CJ production linker.
#ifdef __APPLE__
#define SYM(x) _##x
#define DESCS .section __CJ_METADATA,__cjmethodinfo
#define MAPS .section __CJ_METADATA,__cjstackmap
#else
#define SYM(x) x
#ifdef _WIN64
#define DESCS .section .methodinfo,"dw"
#define MAPS .section .stackmap,"dw"
#else
#if defined(__arm__)
#define DESCS .section .cjmetadata.methodinfo,"aw",%progbits
#define MAPS .section .cjmetadata.stackmap,"aw",%progbits
#else
#define DESCS .section .cjmetadata.methodinfo,"aw",@progbits
#define MAPS .section .cjmetadata.stackmap,"aw",@progbits
#endif
#endif
#endif
.text
.balign 16
.long 0
.globl SYM(ManagedMetadataNative)
SYM(ManagedMetadataNative):
#ifdef _WIN64
.seh_proc ManagedMetadataNative
.seh_endprologue
#endif
.zero 32
#ifdef _WIN64
.seh_endproc
#endif
.macro FUNCTION name, desc
.balign 16
.long \desc - .
.globl \name
\name:
#ifdef _WIN64
.seh_proc \name
.seh_endprologue
#endif
.zero 32
#ifdef _WIN64
.seh_endproc
#endif
.endm
FUNCTION SYM(ManagedMetadataNoMap), .Lnomap_desc
FUNCTION SYM(ManagedMetadataEmpty), .Lempty_desc
FUNCTION SYM(ManagedMetadataZeroRoots), .Lzero_desc
DESCS
.balign 8
.globl SYM(ManagedMetadataDescriptors)
SYM(ManagedMetadataDescriptors):
.macro DESCRIPTOR name, entry, map, qualification
\name:
.long \map
.long 32
.zero 16
#ifdef __APPLE__
.quad 0
.long 1
.long 0
.quad \entry - .
#else
.long 0
.long 1
.long \entry - .
#endif
.long \qualification - .
.long 0x31514a43
#ifndef __APPLE__
.long 0
#endif
.endm
DESCRIPTOR .Lnomap_desc, SYM(ManagedMetadataNoMap), 0, .Lnomap_qualification
DESCRIPTOR .Lempty_desc, SYM(ManagedMetadataEmpty), .Lempty_map-.Lempty_desc, .Lempty_qualification
DESCRIPTOR .Lzero_desc, SYM(ManagedMetadataZeroRoots), .Lzero_map-.Lzero_desc, .Lzero_qualification
MAPS
.balign 8
// Same payloads as the original tests: three zero prologue varints;
// zero records, or one PC=0 row with four zero indices and empty tables.
.globl SYM(ManagedMetadataEmptyMap)
SYM(ManagedMetadataEmptyMap):
.Lempty_map:
.zero 64
.globl SYM(ManagedMetadataZeroMap)
SYM(ManagedMetadataZeroMap):
.Lzero_map:
.byte 0, 0x10, 0x11, 0x11
.zero 60
.macro QUALIFICATION name
.balign 4
\name:
.long 0x31514a43
.long 56
.long 1
.long 4
.long 0, 3
.long 0
.short 3, 0
.long 1
.short 1, 3
.long 4
.short 2, 3
.long 4
.short 3, 0
.endm
QUALIFICATION .Lnomap_qualification
QUALIFICATION .Lempty_qualification
QUALIFICATION .Lzero_qualification
#ifdef _WIN64
.section .drectve
.ascii " -export:ManagedMetadataNative -export:ManagedMetadataNoMap -export:ManagedMetadataEmpty -export:ManagedMetadataZeroRoots -export:ManagedMetadataDescriptors -export:ManagedMetadataEmptyMap -export:ManagedMetadataZeroMap -export:_CJMetadataStart"
#endif
