/* A real assembly sentinel test: CALL/RET must preserve every seeded GPR,
 * RFLAGS, x87/SSE state and a red-zone sentinel across an empty target. */
.text
.globl tiny_target
.type tiny_target,@function
tiny_target:
    ret
.size tiny_target,.-tiny_target
    .p2align 4
.globl abi_target
.type abi_target,@function
abi_target:
    .rept 8
    nop
    .endr
    ret
.size abi_target,.-abi_target

.macro snapshot name
    movq %rax, \name+0(%rip)
    movq %rbx, \name+8(%rip)
    movq %rcx, \name+16(%rip)
    movq %rdx, \name+24(%rip)
    movq %rsi, \name+32(%rip)
    movq %rdi, \name+40(%rip)
    movq %rbp, \name+48(%rip)
    movq %r8, \name+56(%rip)
    movq %r9, \name+64(%rip)
    movq %r10, \name+72(%rip)
    movq %r11, \name+80(%rip)
    movq %r12, \name+88(%rip)
    movq %r13, \name+96(%rip)
    movq %r14, \name+104(%rip)
    movq %r15, \name+112(%rip)
    movq %rsp, \name+120(%rip)
    pushfq
    popq \name+128(%rip)
    fxsave64 \name+160(%rip)
.endm
.globl abi_exercise
.type abi_exercise,@function
abi_exercise:
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    subq $8,%rsp
    movq $0x12345678,-24(%rsp)
    .irp r,rax,rbx,rcx,rdx,rsi,rdi,rbp,r8,r9,r10,r11,r12,r13,r14,r15
    movabsq $0xfedcba9876543210,%\r
    .endr
    .irp r,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    movdqa .Lvector(%rip),%xmm\r
    .endr
    fld1
    pushq $0x647
    popfq
    snapshot abi_before
    call abi_target
    snapshot abi_after
    cld
    fstp %st(0)
    xorl %eax,%eax
    cmpq $0x12345678,-24(%rsp)
    setne %al
    addq $8,%rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    ret
.size abi_exercise,.-abi_exercise
.section .rodata
.p2align 4
.Lvector: .quad 0x0123456789abcdef,0xfedcba9876543210
.bss
.p2align 6
.globl abi_before, abi_after
.hidden abi_before, abi_after
abi_before: .zero 672
.p2align 6
abi_after: .zero 672
.section .note.GNU-stack,"",@progbits
