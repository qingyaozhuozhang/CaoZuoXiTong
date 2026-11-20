/*
 * trampoline.S - position-independent, re-entrant probe trampoline (x86_64).
 *
 * Layout (in this order, all within one copied blob):
 *   [save regs] [record event into ring] [restore regs]
 *   [32-byte relocated-original-instruction slot]
 *   [jmp *ret_addr] [ret_addr][ring_ptr][probe_id]  <- data slots
 *
 * The code is fully RIP-relative, so the whole blob can be memcpy'd to any
 * address in the tracee without fixups (the three data slots are patched by
 * the tracer). It is re-entrant: every register it clobbers is saved on the
 * current thread's own stack, and the ring is a lock-free MPSC queue.
 */

    .text
    .p2align 4
    .globl ludp_tramp_start
    .globl ludp_tramp_end
    .globl ludp_tramp_code_end
    .globl ludp_tramp_data_begin
    .globl ludp_tramp_data_end
    .globl ludp_tramp_reloc_begin
    .globl ludp_tramp_reloc_end
    .globl ludp_tramp_ret_addr
    .globl ludp_tramp_ring_ptr
    .globl ludp_tramp_probe_id
    .globl ludp_tramp_rdfsbase

ludp_tramp_start:
    /* Save every register we may clobber (args + scratch + flags). */
    pushq   %rdi
    pushq   %rsi
    pushq   %rdx
    pushq   %rcx
    pushq   %r8
    pushq   %r9
    pushq   %r10
    pushq   %r11
    pushq   %rax
    pushfq

    /* r8 = ring base, r9 = probe id (both RIP-relative -> position independent) */
    movq    ludp_tramp_ring_ptr(%rip), %r8
    movq    ludp_tramp_probe_id(%rip), %r9

    /* --- reserve one ring slot (lock-free CAS on seq) --- */
1:
    movq    0(%r8), %rax          /* rax = seq */
    movq    %rax, %r10
    addq    $1, %r10
    lock cmpxchgq %r10, 0(%r8)    /* seq <- seq+1 iff still rax */
    je      2f
    pause
    jmp     1b
2:
    movq    %rax, %r11            /* r11 = reserved index i */

    /* --- overflow accounting (informational; the ring simply wraps) --- */
    movq    64(%r8), %rcx         /* consumer watermark */
    movq    %rax, %rdx
    subq    %rcx, %rdx            /* outstanding = i - watermark */
    cmpq    128(%r8), %rdx        /* vs capacity */
    jb      3f
    lock incq 136(%r8)            /* overflow++ */
3:
    andq    144(%r8), %rax        /* rax = idx = i & mask */

    /* event address = ring + events_off + idx*80 */
    movq    %rax, %rcx            /* rcx = idx (kept for the ready flag) */
    imulq   $80, %rax, %rax
    addq    152(%r8), %rax        /* + events_off */
    addq    %r8, %rax
    movq    %rax, %r10            /* r10 = event address */

    /* ready-flag address = ring + READY_OFF + idx*8 */
    shlq    $3, %rcx
    addq    $256, %rcx
    addq    %r8, %rcx             /* rcx = ready-flag address */

    /* tsc */
    rdtsc
    shlq    $32, %rdx
    orq     %rdx, %rax
    movq    %rax, 0(%r10)

    /* per-thread id: TLS base (unique per thread, no syscall) */
ludp_tramp_rdfsbase:
    rdfsbase %rax
    movq    %rax, 8(%r10)

    /* probe id */
    movq    %r9, 16(%r10)

    /* first six SysV args, read back from our own saved stack frame */
    movq    72(%rsp), %rax        /* rdi */
    movq    %rax, 24(%r10)
    movq    64(%rsp), %rax        /* rsi */
    movq    %rax, 32(%r10)
    movq    56(%rsp), %rax        /* rdx */
    movq    %rax, 40(%r10)
    movq    48(%rsp), %rax        /* rcx */
    movq    %rax, 48(%r10)
    movq    40(%rsp), %rax        /* r8  */
    movq    %rax, 56(%r10)
    movq    32(%rsp), %rax        /* r9  */
    movq    %rax, 64(%r10)

    /* return address */
    movq    ludp_tramp_ret_addr(%rip), %rax
    movq    %rax, 72(%r10)

    /* Publish the slot: ready[idx] = i + 1. xchg is a full barrier, so the
     * event payload above is globally visible before the ready flag. There is
     * no commit spin -- producers never wait for one another (F7 scalability). */
    leaq    1(%r11), %rax
    xchgq   %rax, 0(%rcx)

    /* --- restore --- */
    popfq
    popq    %rax
    popq    %r11
    popq    %r10
    popq    %r9
    popq    %r8
    popq    %rcx
    popq    %rdx
    popq    %rsi
    popq    %rdi

    /* 32-byte slot filled by the tracer with the relocated original insns */
ludp_tramp_reloc_begin:
    .space 32, 0x90
ludp_tramp_reloc_end:
    jmp     *ludp_tramp_ret_addr(%rip)

ludp_tramp_code_end:
    .p2align 3
ludp_tramp_data_begin:
ludp_tramp_ret_addr:
    .quad   0
ludp_tramp_ring_ptr:
    .quad   0
ludp_tramp_probe_id:
    .quad   0
ludp_tramp_data_end:
ludp_tramp_end:

    .section .note.GNU-stack,"",@progbits
