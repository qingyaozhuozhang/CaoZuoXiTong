.text
.p2align 4
.globl a4_avx_target
.type a4_avx_target,@function
a4_avx_target:
    endbr64
    nop
    nop
    nop
    nop
    vextracti128 $1, %ymm0, %xmm1
    vmovq %xmm1, %rax
    vzeroupper
    ret
.size a4_avx_target, .-a4_avx_target

.globl a4_avx_invoke
.type a4_avx_invoke,@function
a4_avx_invoke:
    endbr64
    vmovdqu .Lavx_pattern(%rip), %ymm0
    call a4_avx_target
    vzeroupper
    ret
.size a4_avx_invoke, .-a4_avx_invoke

.section .rodata
.p2align 5
.Lavx_pattern:
    .quad 0x1111111111111111
    .quad 0x2222222222222222
    .quad 0x0123456789abcdef
    .quad 0xfedcba9876543210

.section .note.GNU-stack,"",@progbits
