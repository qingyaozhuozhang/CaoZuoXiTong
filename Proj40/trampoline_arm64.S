/* Position-independent AAPCS64 probe code. No per-hit syscalls or traps.
 * Entry saves its scratch registers and NZCV. The original SP, arguments,
 * x8 indirect-result pointer, platform register and FP/SIMD state are intact.
 */
.text
.p2align 4
.globl ludp_tramp_start, ludp_tramp_end, ludp_tramp_code_end
.globl ludp_tramp_data_begin, ludp_tramp_data_end
.globl ludp_tramp_reloc_begin, ludp_tramp_reloc_end
.globl ludp_tramp_ret_addr, ludp_tramp_ring_ptr, ludp_tramp_probe_id
.globl ludp_tramp_slots, ludp_tramp_flags
.globl ludp_return_stub, ludp_return_stub_end

.macro save_scratch
    stp x9, x10, [sp, #-80]!
    stp x11, x12, [sp, #16]
    stp x13, x14, [sp, #32]
    stp x15, x16, [sp, #48]
    mrs x9, nzcv
    stp x17, x9, [sp, #64]
.endm

.macro restore_scratch
    ldp x17, x9, [sp, #64]
    msr nzcv, x9
    ldp x15, x16, [sp, #48]
    ldp x13, x14, [sp, #32]
    ldp x11, x12, [sp, #16]
    ldp x9, x10, [sp], #80
.endm

/* x16 = ring, x17 = optional return slot. Reserve x9 = sequence,
 * x11 = ready word, x12 = payload. Never overwrite an unread slot. */
.macro reserve full
    add x13, x16, #64
    ldr x14, [x16, #128]
.Lreserve\@:
    ldar x10, [x13]
    ldxr x9, [x16]
    sub x15, x9, x10
    cmp x15, x14
    b.hs .Lfull\@
    add x15, x9, #1
    stxr w10, x15, [x16]
    cbnz w10, .Lreserve\@
    ldr x13, [x16, #144]
    and x10, x9, x13
    add x11, x16, x10, lsl #3
    add x11, x11, #256
    ldr x13, [x16, #152]
    add x12, x16, x13
    add x12, x12, x10, lsl #7
    b .Lreserved\@
.Lfull\@:
    clrex
    add x13, x16, #136
    increment x13
    b \full
.Lreserved\@:
.endm

.macro increment address
.Linc\@:
    ldxr x14, [\address]
    add x14, x14, #1
    stxr w15, x14, [\address]
    cbnz w15, .Linc\@
.endm

.macro common_event
    mrs x13, cntvct_el0
    mrs x14, tpidr_el0
    stp x13, x14, [x12]
.endm

.macro publish
    add x9, x9, #1
    stlr x9, [x11]
.endm

ludp_tramp_start:
    save_scratch
    ldr x16, .Lring_ptr
    mov x17, xzr
    ldr x9, .Lflags
    tbz x9, #1, .Lentry_event

    /* A slot stores the original LR and arguments for exactly one call. */
    add x9, sp, #80
    ubfx x9, x9, #4, #12
    ldr x10, .Lslots
    mov x11, #4096
.Lsearch:
    add x17, x10, x9, lsl #8
    add x13, x17, #64
    ldaxr x14, [x13]
    cbnz x14, .Lbusy
    mov x14, #1
    stxr w15, x14, [x13]
    cbz w15, .Lfound
    b .Lnext
.Lbusy:
    clrex
.Lnext:
    add x9, x9, #1
    and x9, x9, #4095
    subs x11, x11, #1
    b.ne .Lsearch
    mov x17, xzr
    add x13, x16, #168
    increment x13
    b .Lentry_event
.Lfound:
    ldr x9, .Lprobe_id
    stp x30, x9, [x17, #72]
    stp x0, x1, [x17, #88]
    stp x2, x3, [x17, #104]
    stp x4, x5, [x17, #120]
    add x13, x16, #176
    increment x13
    str x14, [x17, #136]
    adr x9, .Lreturn_handler
    str x9, [x17, #152]
    mov x30, x17
.Lentry_event:
    reserve .Lentry_done
    common_event
    ldr x13, .Lprobe_id
    str x13, [x12, #16]
    stp x0, x1, [x12, #24]
    stp x2, x3, [x12, #40]
    stp x4, x5, [x12, #56]
    mov x13, x30
    mov x14, xzr
    cbz x17, .Lentry_caller
    ldr x13, [x17, #72]
    ldr x14, [x17, #136]
.Lentry_caller:
    str x14, [x12, #104]
    mov x14, #1
    stp x13, x14, [x12, #72]
    stp xzr, xzr, [x12, #88]
    stp xzr, xzr, [x12, #112]
    publish
.Lentry_done:
    restore_scratch
ludp_tramp_reloc_begin:
    .rept 8
    nop
    .endr
ludp_tramp_reloc_end:
ludp_tramp_ret_addr:
    nop /* Patched to a direct B that preserves every register. */

.Lreturn_handler:
    hint #38 /* BTI JC */
    save_scratch
    ldr x16, .Lring_ptr
    reserve .Lreturn_done
    common_event
    ldr x13, [x17, #80]
    str x13, [x12, #16]
    ldp x13, x14, [x17, #88]
    stp x13, x14, [x12, #24]
    ldp x13, x14, [x17, #104]
    stp x13, x14, [x12, #40]
    ldp x13, x14, [x17, #120]
    stp x13, x14, [x12, #56]
    ldr x13, [x17, #72]
    mov x14, #2
    stp x13, x14, [x12, #72]
    stp x0, x1, [x12, #88]
    ldr x13, [x17, #136]
    str x13, [x12, #104]
    str q0, [x12, #112]
    publish
.Lreturn_done:
    ldr x30, [x17, #72]
    add x13, x17, #64
    stlr xzr, [x13]
    restore_scratch
    ldp x16, x17, [sp], #16
    ret
ludp_tramp_code_end:

.p2align 3
ludp_tramp_data_begin:
ludp_tramp_ring_ptr:
.Lring_ptr: .quad 0
ludp_tramp_probe_id:
.Lprobe_id: .quad 0
ludp_tramp_slots:
.Lslots: .quad 0
ludp_tramp_flags:
.Lflags: .quad 0
ludp_tramp_data_end:
ludp_tramp_end:

.p2align 4
ludp_return_stub:
.Lstub:
    hint #38 /* BTI JC */
    stp x16, x17, [sp, #-16]!
    adr x17, .Lstub
    ldr x16, [x17, #152]
    br x16
    .rept (64-(.-.Lstub))/4
    nop
    .endr
ludp_return_stub_end:
.section .note.GNU-stack,"",%progbits
