// Synthetic compiler metadata input, not a copy of an unwind implementation.
.text
.p2align 4
.long 0
.globl MetadataNoDescriptor
.seh_proc MetadataNoDescriptor
MetadataNoDescriptor:
.seh_endprologue
nop
nop
ret
.seh_endproc
.p2align 4
.Lnomap_slot:
.long .Lnomap_desc - .Lnomap_slot
.globl MetadataNoMap
.seh_proc MetadataNoMap
MetadataNoMap:
.seh_endprologue
nop
nop
ret
.seh_endproc
.p2align 4
.Lpresent_slot:
.long .Lpresent_desc - .Lpresent_slot
.globl MetadataPresent
.seh_proc MetadataPresent
MetadataPresent:
.seh_endprologue
nop
nop
ret
.seh_endproc
.data
.p2align 3
.Lnomap_desc:
.zero 32
.Lpresent_desc:
.long .Lmap - .Lpresent_desc
.zero 28
.p2align 3
.Lmap:
.zero 64
