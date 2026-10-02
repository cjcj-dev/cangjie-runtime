/* Linux ELF fixture only. Intent is qualified after linking/loading. */
PHDRS {
  headers PT_PHDR PHDRS;
  ro PT_LOAD FILEHDR PHDRS FLAGS(4);
  text PT_LOAD FLAGS(5);
  data PT_LOAD FLAGS(6);
  dynamic PT_DYNAMIC FLAGS(6);
  left PT_LOAD FLAGS(4);
  right PT_LOAD FLAGS(5);
  stack PT_GNU_STACK FLAGS(6);
}
SECTIONS {
  . = SIZEOF_HEADERS;
  .note.gnu.build-id : { *(.note.gnu.build-id) } :ro
  .gnu.hash : { *(.gnu.hash) } :ro
  .hash : { *(.hash) } :ro
  .dynsym : { *(.dynsym) } :ro
  .dynstr : { *(.dynstr) } :ro
  .gnu.version : { *(.gnu.version) } :ro
  .gnu.version_d : { *(.gnu.version_d) } :ro
  .gnu.version_r : { *(.gnu.version_r) } :ro
  .rela.dyn : { *(.rela.dyn .rela.data .rela.data.* .rela.got .rela.bss .rela.ifunc) } :ro
  .rela.plt : { *(.rela.plt .rela.iplt) } :ro
  . = ALIGN(0x1000);
  .init : { *(.init) } :text
  .plt : { *(.plt) *(.plt.*) } :text
  .text : { *(.text .text.*) } :text
  .fini : { *(.fini) } :text
  .gc_unit_metadata : { *(.gc_unit_metadata) } :text
  .rodata : { *(.rodata .rodata.*) } :text
  .eh_frame_hdr : { *(.eh_frame_hdr) } :text
  .eh_frame : { KEEP(*(.eh_frame)) } :text
  . = ALIGN(0x1000);
  .init_array : { KEEP(*(.init_array .init_array.*)) } :data
  .fini_array : { KEEP(*(.fini_array .fini_array.*)) } :data
  .dynamic : { *(.dynamic) } :data :dynamic
  .got : { *(.got .got.*) } :data
  .data : { *(.data .data.*) } :data
  .bss : { *(.bss .bss.*) *(COMMON) } :data
  ASSERT(. < 0x400000, "ordinary fixture sections overlap contiguous input")
  .a2_left 0x400000 : { KEEP(*(.a2_left)) } :left
  .a2_right 0x401000 : { KEEP(*(.a2_right)) } :right
  ASSERT(SIZEOF(.a2_left) == 4096, "left fixture section changed")
  /DISCARD/ : { *(.note.GNU-stack) }
}
