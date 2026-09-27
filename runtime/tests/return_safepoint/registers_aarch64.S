// Direct bridge; x18 retains the Apple platform register value.
#ifdef __APPLE__
#define SYM(x) _##x
#else
#define SYM(x) x
#endif
.text
.p2align 2
.globl SYM(CheckReturnStub)
SYM(CheckReturnStub):
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    sub sp, sp, #512
    str x1, [sp]
    str x19, [sp, #16]
    str x20, [sp, #24]
    str x21, [sp, #32]
    str x22, [sp, #40]
    str x23, [sp, #48]
    str x24, [sp, #56]
    str x25, [sp, #64]
    str x26, [sp, #72]
    str x27, [sp, #80]
    str x28, [sp, #88]
    str q8, [sp, #112]
    str q9, [sp, #128]
    str q10, [sp, #144]
    str q11, [sp, #160]
    str q12, [sp, #176]
    str q13, [sp, #192]
    str q14, [sp, #208]
    str q15, [sp, #224]
    mov x28, x0
    mov x0, #6016
    mov x1, #6017
    mov x2, #6018
    mov x3, #6019
    mov x4, #6020
    mov x5, #6021
    mov x6, #6022
    mov x7, #6023
    mov x8, #6024
    mov x19, #6025
    mov x20, #6026
    mov x21, #6027
    mov x22, #6028
    mov x23, #6029
    mov x24, #6030
    mov x25, #6031
    mov x26, #6032
    mov x27, #6033
    movi v0.16b, #1
    movi v1.16b, #2
    movi v2.16b, #3
    movi v3.16b, #4
    movi v4.16b, #5
    movi v5.16b, #6
    movi v6.16b, #7
    movi v7.16b, #8
    movi v8.16b, #9
    movi v9.16b, #10
    movi v10.16b, #11
    movi v11.16b, #12
    movi v12.16b, #13
    movi v13.16b, #14
    movi v14.16b, #15
    movi v15.16b, #16
    movi v16.16b, #17
    movi v17.16b, #18
    movi v18.16b, #19
    movi v19.16b, #20
    movi v20.16b, #21
    movi v21.16b, #22
    movi v22.16b, #23
    movi v23.16b, #24
    movi v24.16b, #25
    movi v25.16b, #26
    movi v26.16b, #27
    movi v27.16b, #28
    movi v28.16b, #29
    movi v29.16b, #30
    movi v30.16b, #31
    movi v31.16b, #32
    ldr x9, [sp]
    str x0, [x9, #0]
    str x1, [x9, #8]
    str x2, [x9, #16]
    str x3, [x9, #24]
    str x4, [x9, #32]
    str x5, [x9, #40]
    str x6, [x9, #48]
    str x7, [x9, #56]
    str x8, [x9, #64]
    str x19, [x9, #72]
    str x20, [x9, #80]
    str x21, [x9, #88]
    str x22, [x9, #96]
    str x23, [x9, #104]
    str x24, [x9, #112]
    str x25, [x9, #120]
    str x26, [x9, #128]
    str x27, [x9, #136]
    str x28, [x9, #144]
    str x29, [x9, #152]
    str q0, [x9, #320]
    str q1, [x9, #336]
    str q2, [x9, #352]
    str q3, [x9, #368]
    str q4, [x9, #384]
    str q5, [x9, #400]
    str q6, [x9, #416]
    str q7, [x9, #432]
    str q8, [x9, #448]
    str q9, [x9, #464]
    str q10, [x9, #480]
    str q11, [x9, #496]
    str q12, [x9, #512]
    str q13, [x9, #528]
    str q14, [x9, #544]
    str q15, [x9, #560]
    str q16, [x9, #576]
    str q17, [x9, #592]
    str q18, [x9, #608]
    str q19, [x9, #624]
    str q20, [x9, #640]
    str q21, [x9, #656]
    str q22, [x9, #672]
    str q23, [x9, #688]
    str q24, [x9, #704]
    str q25, [x9, #720]
    str q26, [x9, #736]
    str q27, [x9, #752]
    str q28, [x9, #768]
    str q29, [x9, #784]
    str q30, [x9, #800]
    str q31, [x9, #816]
    mov x16, #0
    mov x17, #0
    bl SYM(CJ_MCC_HandleReturnSafepoint)
    ldr x9, [sp]
    str x0, [x9, #160]
    str x1, [x9, #168]
    str x2, [x9, #176]
    str x3, [x9, #184]
    str x4, [x9, #192]
    str x5, [x9, #200]
    str x6, [x9, #208]
    str x7, [x9, #216]
    str x8, [x9, #224]
    str x19, [x9, #232]
    str x20, [x9, #240]
    str x21, [x9, #248]
    str x22, [x9, #256]
    str x23, [x9, #264]
    str x24, [x9, #272]
    str x25, [x9, #280]
    str x26, [x9, #288]
    str x27, [x9, #296]
    str x28, [x9, #304]
    str x29, [x9, #312]
    str q0, [x9, #832]
    str q1, [x9, #848]
    str q2, [x9, #864]
    str q3, [x9, #880]
    str q4, [x9, #896]
    str q5, [x9, #912]
    str q6, [x9, #928]
    str q7, [x9, #944]
    str q8, [x9, #960]
    str q9, [x9, #976]
    str q10, [x9, #992]
    str q11, [x9, #1008]
    str q12, [x9, #1024]
    str q13, [x9, #1040]
    str q14, [x9, #1056]
    str q15, [x9, #1072]
    str q16, [x9, #1088]
    str q17, [x9, #1104]
    str q18, [x9, #1120]
    str q19, [x9, #1136]
    str q20, [x9, #1152]
    str q21, [x9, #1168]
    str q22, [x9, #1184]
    str q23, [x9, #1200]
    str q24, [x9, #1216]
    str q25, [x9, #1232]
    str q26, [x9, #1248]
    str q27, [x9, #1264]
    str q28, [x9, #1280]
    str q29, [x9, #1296]
    str q30, [x9, #1312]
    str q31, [x9, #1328]
    ldr q8, [sp, #112]
    ldr q9, [sp, #128]
    ldr q10, [sp, #144]
    ldr q11, [sp, #160]
    ldr q12, [sp, #176]
    ldr q13, [sp, #192]
    ldr q14, [sp, #208]
    ldr q15, [sp, #224]
    ldr x19, [sp, #16]
    ldr x20, [sp, #24]
    ldr x21, [sp, #32]
    ldr x22, [sp, #40]
    ldr x23, [sp, #48]
    ldr x24, [sp, #56]
    ldr x25, [sp, #64]
    ldr x26, [sp, #72]
    ldr x27, [sp, #80]
    ldr x28, [sp, #88]
    add sp, sp, #512
    ldp x29, x30, [sp], #16
    ret
