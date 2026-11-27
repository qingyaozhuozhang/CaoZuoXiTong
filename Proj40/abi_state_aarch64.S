/* AAPCS64 sentinel: all GPRs, SP, NZCV, FPCR/FPSR and all 128-bit SIMD
 * registers must survive an empty probed target. The incoming LR is set to
 * BL's continuation before the first snapshot, so both LR values match. */
.text
.p2align 2
.globl tiny_target
.type tiny_target,%function
tiny_target:
    svc #0                       // Unsupported entry, never executed.
    ret
.size tiny_target,.-tiny_target

.globl abi_target
.type abi_target,%function
abi_target:
    .rept 8
    nop
    .endr
    ret
.size abi_target,.-abi_target

.macro snapshot name
    stp x16, x17, [sp, #-16]!
    adrp x16, \name
    add x16, x16, :lo12:\name
    stp x0, x1, [x16, #0]
    stp x2, x3, [x16, #16]
    stp x4, x5, [x16, #32]
    stp x6, x7, [x16, #48]
    stp x8, x9, [x16, #64]
    stp x10, x11, [x16, #80]
    stp x12, x13, [x16, #96]
    stp x14, x15, [x16, #112]
    ldr x17, [sp]
    str x17, [x16, #128]
    ldr x17, [sp, #8]
    str x17, [x16, #136]
    stp x18, x19, [x16, #144]
    stp x20, x21, [x16, #160]
    stp x22, x23, [x16, #176]
    stp x24, x25, [x16, #192]
    stp x26, x27, [x16, #208]
    stp x28, x29, [x16, #224]
    str x30, [x16, #240]
    add x17, sp, #16
    str x17, [x16, #248]
    mrs x17, nzcv
    str x17, [x16, #256]
    mrs x17, fpcr
    str x17, [x16, #264]
    mrs x17, fpsr
    str x17, [x16, #272]
    stp q0, q1, [x16, #288]
    stp q2, q3, [x16, #320]
    stp q4, q5, [x16, #352]
    stp q6, q7, [x16, #384]
    stp q8, q9, [x16, #416]
    stp q10, q11, [x16, #448]
    stp q12, q13, [x16, #480]
    stp q14, q15, [x16, #512]
    stp q16, q17, [x16, #544]
    stp q18, q19, [x16, #576]
    stp q20, q21, [x16, #608]
    stp q22, q23, [x16, #640]
    stp q24, q25, [x16, #672]
    stp q26, q27, [x16, #704]
    stp q28, q29, [x16, #736]
    stp q30, q31, [x16, #768]
    ldp x16, x17, [sp], #16
.endm

.globl abi_exercise
.type abi_exercise,%function
abi_exercise:
    sub sp, sp, #256
    stp x19, x20, [sp]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    str x18, [sp, #96]
    stp q8, q9, [sp, #112]
    stp q10, q11, [sp, #144]
    stp q12, q13, [sp, #176]
    stp q14, q15, [sp, #208]
    mrs x0, fpcr
    mrs x1, fpsr
    stp x0, x1, [sp, #240]
    mov x0, #0x400000
    msr fpcr, x0
    mov x0, #0x10
    msr fpsr, x0
    mov x0, #0xf0000000
    msr nzcv, x0
    movz x0, #0x3210
    movk x0, #0x7654, lsl #16
    movk x0, #0xba98, lsl #32
    movk x0, #0xfedc, lsl #48
    .irp r,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29
    mov x\r, #\r
    .endr
    .irp r,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29
    dup v\r\().2d, x\r
    .endr
    mov x30, #30
    dup v30.2d, x30
    mov x30, #31
    dup v31.2d, x30
    adr x30, .Lafter_call
    snapshot abi_before
    bl abi_target
.Lafter_call:
    snapshot abi_after
    ldp x0, x1, [sp, #240]
    msr fpcr, x0
    msr fpsr, x1
    ldp q8, q9, [sp, #112]
    ldp q10, q11, [sp, #144]
    ldp q12, q13, [sp, #176]
    ldp q14, q15, [sp, #208]
    ldr x18, [sp, #96]
    ldp x19, x20, [sp]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    add sp, sp, #256
    mov w0, #0
    ret
.size abi_exercise,.-abi_exercise

.bss
.p2align 6
.globl abi_before, abi_after
.hidden abi_before, abi_after
abi_before: .zero 800
.p2align 6
abi_after: .zero 800
.section .note.GNU-stack,"",%progbits
