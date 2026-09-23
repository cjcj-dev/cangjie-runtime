/*
 * Copyright (c) 2003, 2024, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2014, Red Hat Inc. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

// Atomic word-copy subset; included inside class Copy.
// cpu/aarch64/copy_aarch64.hpp:57-137,161-167
#define COPY_SMALL(from, to, count)                                     \
{                                                                       \
        long tmp0, tmp1, tmp2, tmp3;                                    \
        long tmp4, tmp5, tmp6, tmp7;                                    \
  __asm volatile(                                                       \
"       adr     %[t0], 0f;\n"                                           \
"       add     %[t0], %[t0], %[cnt], lsl #5;\n"                        \
"       br      %[t0];\n"                                               \
"       .align  5;\n"                                                   \
"0:"                                                                    \
"       hint    #0x24; // bti j\n"                                      \
"       b       1f;\n"                                                  \
"       .align  5;\n"                                                   \
"       hint    #0x24; // bti j\n"                                      \
"       ldr     %[t0], [%[s], #0];\n"                                   \
"       str     %[t0], [%[d], #0];\n"                                   \
"       b       1f;\n"                                                  \
"       .align  5;\n"                                                   \
"       hint    #0x24; // bti j\n"                                      \
"       ldp     %[t0], %[t1], [%[s], #0];\n"                            \
"       stp     %[t0], %[t1], [%[d], #0];\n"                            \
"       b       1f;\n"                                                  \
"       .align  5;\n"                                                   \
"       hint    #0x24; // bti j\n"                                      \
"       ldp     %[t0], %[t1], [%[s], #0];\n"                            \
"       ldr     %[t2], [%[s], #16];\n"                                  \
"       stp     %[t0], %[t1], [%[d], #0];\n"                            \
"       str     %[t2], [%[d], #16];\n"                                  \
"       b       1f;\n"                                                  \
"       .align  5;\n"                                                   \
"       hint    #0x24; // bti j\n"                                      \
"       ldp     %[t0], %[t1], [%[s], #0];\n"                            \
"       ldp     %[t2], %[t3], [%[s], #16];\n"                           \
"       stp     %[t0], %[t1], [%[d], #0];\n"                            \
"       stp     %[t2], %[t3], [%[d], #16];\n"                           \
"       b       1f;\n"                                                  \
"       .align  5;\n"                                                   \
"       hint    #0x24; // bti j\n"                                      \
"       ldp     %[t0], %[t1], [%[s], #0];\n"                            \
"       ldp     %[t2], %[t3], [%[s], #16];\n"                           \
"       ldr     %[t4], [%[s], #32];\n"                                  \
"       stp     %[t0], %[t1], [%[d], #0];\n"                            \
"       stp     %[t2], %[t3], [%[d], #16];\n"                           \
"       str     %[t4], [%[d], #32];\n"                                  \
"       b       1f;\n"                                                  \
"       .align  5;\n"                                                   \
"       hint    #0x24; // bti j\n"                                      \
"       ldp     %[t0], %[t1], [%[s], #0];\n"                            \
"       ldp     %[t2], %[t3], [%[s], #16];\n"                           \
"       ldp     %[t4], %[t5], [%[s], #32];\n"                           \
"2:"                                                                    \
"       stp     %[t0], %[t1], [%[d], #0];\n"                            \
"       stp     %[t2], %[t3], [%[d], #16];\n"                           \
"       stp     %[t4], %[t5], [%[d], #32];\n"                           \
"       b       1f;\n"                                                  \
"       .align  5;\n"                                                   \
"       hint    #0x24; // bti j\n"                                      \
"       ldr     %[t6], [%[s], #0];\n"                                   \
"       ldp     %[t0], %[t1], [%[s], #8];\n"                            \
"       ldp     %[t2], %[t3], [%[s], #24];\n"                           \
"       ldp     %[t4], %[t5], [%[s], #40];\n"                           \
"       str     %[t6], [%[d]], #8;\n"                                   \
"       b       2b;\n"                                                  \
"       .align  5;\n"                                                   \
"       hint    #0x24; // bti j\n"                                      \
"       ldp     %[t0], %[t1], [%[s], #0];\n"                            \
"       ldp     %[t2], %[t3], [%[s], #16];\n"                           \
"       ldp     %[t4], %[t5], [%[s], #32];\n"                           \
"       ldp     %[t6], %[t7], [%[s], #48];\n"                           \
"       stp     %[t0], %[t1], [%[d], #0];\n"                            \
"       stp     %[t2], %[t3], [%[d], #16];\n"                           \
"       stp     %[t4], %[t5], [%[d], #32];\n"                           \
"       stp     %[t6], %[t7], [%[d], #48];\n"                           \
"1:"                                                                    \
                                                                        \
  : [s]"+r"(from), [d]"+r"(to), [cnt]"+r"(count),                       \
    [t0]"=&r"(tmp0), [t1]"=&r"(tmp1), [t2]"=&r"(tmp2), [t3]"=&r"(tmp3), \
    [t4]"=&r"(tmp4), [t5]"=&r"(tmp5), [t6]"=&r"(tmp6), [t7]"=&r"(tmp7)  \
  :                                                                     \
  : "memory", "cc");                                                    \
}

static void pd_disjoint_words_atomic(const uintptr_t* from, uintptr_t* to, size_t count) {
  __asm volatile( "prfm pldl1strm, [%[s], #0];" :: [s]"r"(from) : "memory");
  if (__builtin_expect(count <= 8, 1)) {
    COPY_SMALL(from, to, count);
    return;
  }
  MRT_CopyDisjointWords(from, to, count);
}


#undef COPY_SMALL
