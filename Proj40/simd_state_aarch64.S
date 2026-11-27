/* A4 must restore SIMD lanes before the original target consumes them. */
.text
.p2align 2
.globl a4_vector_target
.type a4_vector_target,%function
a4_vector_target:
    .rept 8
    nop
    .endr
    movz x2, #0xcdef
    movk x2, #0x89ab, lsl #16
    movk x2, #0x4567, lsl #32
    movk x2, #0x0123, lsl #48
    mov x0, x2
    .irp r,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    umov x1, v\r\().d[1]
    cmp x1, x2
    csel x0, x0, xzr, eq
    .endr
    ret
.size a4_vector_target,.-a4_vector_target

.globl a4_vector_invoke
.type a4_vector_invoke,%function
a4_vector_invoke:
    stp x29, x30, [sp, #-144]!
    stp q8, q9, [sp, #16]
    stp q10, q11, [sp, #48]
    stp q12, q13, [sp, #80]
    stp q14, q15, [sp, #112]
    adrp x0, .Lsimd_pattern
    add x0, x0, :lo12:.Lsimd_pattern
    .irp r,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    ldr q\r, [x0]
    .endr
    bl a4_vector_target
    ldp q8, q9, [sp, #16]
    ldp q10, q11, [sp, #48]
    ldp q12, q13, [sp, #80]
    ldp q14, q15, [sp, #112]
    ldp x29, x30, [sp], #144
    ret
.size a4_vector_invoke,.-a4_vector_invoke
.section .rodata
.p2align 4
.Lsimd_pattern:
    .quad 0x1111111111111111
    .quad 0x0123456789abcdef
.section .note.GNU-stack,"",%progbits
