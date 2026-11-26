/* Check the registers used by the ARM64 trampoline, including NZCV and
 * the indirect-result register. The first instruction after BTI is PC-relative.
 */
.text
.globl tiny_target
.type tiny_target,%function
tiny_target:
    ret
.size tiny_target,.-tiny_target

.p2align 4
.globl abi_target
.type abi_target,%function
abi_target:
    hint #34 /* BTI C */
    adr x9, .Lanchor
    ret
.size abi_target,.-abi_target
.Lanchor:

.macro snapshot buffer
    adr x19, \buffer
    stp x0, x8, [x19]
    stp x9, x10, [x19, #16]
    stp x11, x12, [x19, #32]
    stp x13, x14, [x19, #48]
    stp x15, x16, [x19, #64]
    mov x20, sp
    stp x17, x20, [x19, #80]
    mrs x20, nzcv
    str x20, [x19, #96]
    stp q0, q1, [x19, #112]
    str q16, [x19, #144]
.endm

.globl abi_exercise
.type abi_exercise,%function
abi_exercise:
    stp x19, x20, [sp, #-32]!
    str x30, [sp, #16]
    movz x0, #0x3210
    movk x0, #0x7654, lsl #16
    movk x0, #0xba98, lsl #32
    movk x0, #0xfedc, lsl #48
    mov x8, #8
    adr x9, .Lanchor
    mov x10, #10
    mov x11, #11
    mov x12, #12
    mov x13, #13
    mov x14, #14
    mov x15, #15
    mov x16, #16
    mov x17, #17
    movi v0.16b, #0x31
    movi v1.16b, #0x32
    movi v16.16b, #0x33
    cmp x0, x0
    snapshot abi_before
    bl abi_target
    snapshot abi_after
    mov w0, wzr
    ldr x30, [sp, #16]
    ldp x19, x20, [sp], #32
    ret
.size abi_exercise,.-abi_exercise

.bss
.p2align 4
.globl abi_before, abi_after
.hidden abi_before, abi_after
abi_before: .zero 672
.p2align 4
abi_after: .zero 672
.section .note.GNU-stack,"",%progbits
