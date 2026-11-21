/* Copied, RIP-relative x86-64 code. No calls, syscalls, traps or FP mutations.
 * Both paths preserve the 128-byte SysV red zone. Event publication is a plain
 * aligned store (x86 TSO release); the consumer owns slots until its watermark.
 */
.text
.p2align 4
.globl ludp_tramp_start, ludp_tramp_end, ludp_tramp_code_end
.globl ludp_tramp_data_begin, ludp_tramp_data_end
.globl ludp_tramp_reloc_begin, ludp_tramp_reloc_end
.globl ludp_tramp_ret_addr, ludp_tramp_ring_ptr, ludp_tramp_probe_id
.globl ludp_tramp_rdfsbase, ludp_tramp_return_rdfsbase
.globl ludp_tramp_slots, ludp_tramp_flags
.globl ludp_return_stub, ludp_return_stub_end

/* r8=ring; uses rax,rcx,rdx,r10,r11; jumps to fail when full.
 * Never overwrite a live slot, including when an older writer is preempted. */
.macro reserve fail
.Lretry\@:
    movq 64(%r8), %rdx
    movq 0(%r8), %rax
    movq %rax, %r10
    subq %rdx, %r10
    cmpq 128(%r8), %r10
    jae .Lfull\@
    leaq 1(%rax), %r10
    lock cmpxchgq %r10, 0(%r8)
    jne .Lretry\@
    movq %rax, %r11
    andq 144(%r8), %rax
    leaq 256(%r8,%rax,8), %rcx
    shlq $7, %rax
    addq 152(%r8), %rax
    leaq (%r8,%rax), %r10
    jmp .Lreserved\@
.Lfull\@:
    lock incq 136(%r8)
    jmp \fail
.Lreserved\@:
.endm
.macro timestamp
    rdtsc
    shlq $32, %rdx
    orq %rdx, %rax
    movq %rax, 0(%r10)
.endm
.macro publish
    leaq 1(%r11), %rax
    movq %rax, (%rcx)
.endm

ludp_tramp_start:
    endbr64
    leaq -128(%rsp), %rsp
    pushq %rdi
    pushq %rsi
    pushq %rdx
    pushq %rcx
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %rax
    pushfq
    movq ludp_tramp_ring_ptr(%rip), %r8
    xorq %rdi, %rdi
    testq $2, ludp_tramp_flags(%rip)
    jz .Lentry_event

    /* Hash the return-slot address, then bounded linear probing. No shared
     * per-thread stack or spinning on another thread/signal handler. */
    leaq 208(%rsp), %r10
    shrq $4, %r10
    andq $4095, %r10
    movq $4096, %r9
.Lslot_search:
    movq %r10, %rsi
    shlq $8, %rsi
    addq ludp_tramp_slots(%rip), %rsi
    xorl %eax, %eax
    movq $1, %rdx
    lock cmpxchgq %rdx, 64(%rsi)
    je .Lslot_found
    incq %r10
    andq $4095, %r10
    decq %r9
    jnz .Lslot_search
    lock incq 168(%r8)
    jmp .Lentry_event
.Lslot_found:
    movq %rsi, %rdi
    movq 208(%rsp), %rax
    movq %rax, 72(%rdi)
    movq ludp_tramp_probe_id(%rip), %rax
    movq %rax, 80(%rdi)
    movq 72(%rsp), %rax
    movq %rax, 88(%rdi)
    movq 64(%rsp), %rax
    movq %rax, 96(%rdi)
    movq 56(%rsp), %rax
    movq %rax, 104(%rdi)
    movq 48(%rsp), %rax
    movq %rax, 112(%rdi)
    movq 40(%rsp), %rax
    movq %rax, 120(%rdi)
    movq 32(%rsp), %rax
    movq %rax, 128(%rdi)
    movq $1, %rax
    lock xaddq %rax, 176(%r8)
    incq %rax
    movq %rax, 136(%rdi)
    leaq .Lreturn_handler(%rip), %rax
    movq %rax, 152(%rdi)
    movq %rdi, 208(%rsp)
.Lentry_event:
    reserve .Lentry_restore
    timestamp
ludp_tramp_rdfsbase:
    rdfsbase %rax
    movq %rax, 8(%r10)
    movq ludp_tramp_probe_id(%rip), %rax
    movq %rax, 16(%r10)
    movq 72(%rsp), %rax
    movq %rax, 24(%r10)
    movq 64(%rsp), %rax
    movq %rax, 32(%r10)
    movq 56(%rsp), %rax
    movq %rax, 40(%r10)
    movq 48(%rsp), %rax
    movq %rax, 48(%r10)
    movq 40(%rsp), %rax
    movq %rax, 56(%r10)
    movq 32(%rsp), %rax
    movq %rax, 64(%r10)
    movq $0, 104(%r10)
    movq 208(%rsp), %rax
    testq %rdi, %rdi
    jz .Lcaller
    movq 136(%rdi), %rax
    movq %rax, 104(%r10)
    movq 72(%rdi), %rax
.Lcaller:
    movq %rax, 72(%r10)
    movq $1, 80(%r10)
    movq $0, 88(%r10)
    movq $0, 96(%r10)
    movq $0, 112(%r10)
    movq $0, 120(%r10)
    publish
.Lentry_restore:
    popfq
    popq %rax
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rcx
    popq %rdx
    popq %rsi
    popq %rdi
    leaq 128(%rsp), %rsp
ludp_tramp_reloc_begin:
    .space 32, 0x90
ludp_tramp_reloc_end:
    jmp *ludp_tramp_ret_addr(%rip)

/* Unique stub has saved original r11 below the caller's red zone. */
.Lreturn_handler:
    endbr64
    pushq %rdi
    pushq %rsi
    pushq %rdx
    pushq %rcx
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %rax
    pushfq
    movq %r11, %rdi
    movq ludp_tramp_ring_ptr(%rip), %r8
    reserve .Lreturn_restore
    timestamp
ludp_tramp_return_rdfsbase:
    rdfsbase %rax
    movq %rax, 8(%r10)
    movq 80(%rdi), %rax
    movq %rax, 16(%r10)
    movq 88(%rdi), %rax
    movq %rax, 24(%r10)
    movq 96(%rdi), %rax
    movq %rax, 32(%r10)
    movq 104(%rdi), %rax
    movq %rax, 40(%r10)
    movq 112(%rdi), %rax
    movq %rax, 48(%r10)
    movq 120(%rdi), %rax
    movq %rax, 56(%r10)
    movq 128(%rdi), %rax
    movq %rax, 64(%r10)
    movq 72(%rdi), %rax
    movq %rax, 72(%r10)
    movq $2, 80(%r10)
    movq 8(%rsp), %rax
    movq %rax, 88(%r10)
    movq 48(%rsp), %rax
    movq %rax, 96(%r10)
    movq 136(%rdi), %rax
    movq %rax, 104(%r10)
    movdqu %xmm0, 112(%r10)
    publish
.Lreturn_restore:
    movq 72(%rdi), %rax
    movq %rax, 200(%rsp)
    movq $0, 64(%rdi)
    popfq
    popq %rax
    popq %r10
    popq %r9
    popq %r8
    popq %rcx
    popq %rdx
    popq %rsi
    popq %rdi
    popq %r11
    leaq 128(%rsp), %rsp
    jmp *-8(%rsp)
ludp_tramp_code_end:
.p2align 3
ludp_tramp_data_begin:
ludp_tramp_ret_addr: .quad 0
ludp_tramp_ring_ptr: .quad 0
ludp_tramp_probe_id: .quad 0
ludp_tramp_slots: .quad 0
ludp_tramp_flags: .quad 0
ludp_tramp_data_end:
ludp_tramp_end:

.p2align 4
ludp_return_stub:
    endbr64
    leaq -128(%rsp), %rsp
    pushq %r11
    leaq ludp_return_stub(%rip), %r11
    jmp *152(%r11)
    .fill 64-(.-ludp_return_stub),1,0x90
ludp_return_stub_end:
.section .note.GNU-stack,"",@progbits
