/* Position-independent Linux AAPCS64 trampoline. Per-hit processing stays in
 * userspace. All GPRs and NZCV are restored; A4 also preserves Q0-Q31 and
 * FPCR/FPSR. SP is 16-byte aligned at every public call boundary.
 * Scalable-vector/SME state is outside this fixed-width SIMD backend. */
.text
.p2align 4
.globl ludp_tramp_start, ludp_tramp_end, ludp_tramp_code_end
.globl ludp_tramp_data_begin, ludp_tramp_data_end
.globl ludp_tramp_reloc_begin, ludp_tramp_reloc_end, ludp_tramp_exit
.globl ludp_tramp_ret_addr, ludp_tramp_ring_ptr, ludp_tramp_probe_id
.globl ludp_tramp_rdfsbase, ludp_tramp_return_rdfsbase
.globl ludp_tramp_slots, ludp_tramp_flags
.globl ludp_tramp_filter_op, ludp_tramp_filter_arg
.globl ludp_tramp_filter_value, ludp_tramp_filter_mask
.globl ludp_tramp_call_fn, ludp_tramp_call_data, ludp_tramp_call_active
.globl ludp_tramp_xsave_mode, ludp_tramp_xsave_size, ludp_tramp_xsave_mask
.globl ludp_return_stub, ludp_return_stub_end

/* The register frame is x0..x30 at 0..240, then NZCV at 248. */
.macro save_gprs
    sub sp, sp, #256
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    mrs x9, nzcv
    stp x30, x9, [sp, #240]
.endm

.macro restore_gprs
    ldp x30, x9, [sp, #240]
    msr nzcv, x9
    ldp x28, x29, [sp, #224]
    ldp x26, x27, [sp, #208]
    ldp x24, x25, [sp, #192]
    ldp x22, x23, [sp, #176]
    ldp x20, x21, [sp, #160]
    ldp x18, x19, [sp, #144]
    ldp x16, x17, [sp, #128]
    ldp x14, x15, [sp, #112]
    ldp x12, x13, [sp, #96]
    ldp x10, x11, [sp, #80]
    ldp x8, x9, [sp, #64]
    ldp x6, x7, [sp, #48]
    ldp x4, x5, [sp, #32]
    ldp x2, x3, [sp, #16]
    ldp x0, x1, [sp, #0]
    add sp, sp, #256
.endm

/* Atomically add signed immediate to *address. Uses x13, w15.  These are
 * counters, not publication locks: payload visibility is carried by the
 * ready-slot STLR/reader acquire pair below, so relaxed exclusives avoid
 * redundant barriers on the hit path. */
.macro atomic_add address, amount
.Latomic\@:
    ldxr x13, [\address]
    add x13, x13, #\amount
    stxr w15, x13, [\address]
    cbnz w15, .Latomic\@
.endm

/* x20=ring, x21=optional return slot. Result: x10=event, x11=ready,
 * x12=sequence. A stale committed watermark can only cause a conservative
 * drop; the sequence reservation itself needs atomicity but no ordering.
 * Payload publication remains a release store in publish. */
.macro reserve fail
.Lreserve\@:
    add x16, x20, #64
    ldr x9, [x16]
    ldxr x8, [x20]
    sub x13, x8, x9
    ldr x14, [x20, #128]
    cmp x13, x14
    b.hs .Lfull\@
    add x13, x8, #1
    stxr w15, x13, [x20]
    cbnz w15, .Lreserve\@
    mov x12, x8
    ldr x14, [x20, #144]
    and x8, x8, x14
    add x11, x20, #256
    add x11, x11, x8, lsl #3
    ldr x14, [x20, #152]
    add x10, x20, x14
    add x10, x10, x8, lsl #7
    b .Lreserved\@
.Lfull\@:
    clrex
    add x16, x20, #136
    atomic_add x16, 1
    b \fail
.Lreserved\@:
.endm

.macro publish
    add x13, x12, #1
    stlr x13, [x11]
.endm

ludp_tramp_start:
    save_gprs
    ldr x8, ludp_tramp_filter_op
    cbz x8, .Lfilter_pass
    ldr x9, ludp_tramp_filter_arg
    cmp x9, #5
    b.hi .Lentry_restore
    ldr x10, [sp, x9, lsl #3]
    ldr x11, ludp_tramp_filter_value
    cmp x8, #3
    b.ne .Lfilter_choose
    ldr x9, ludp_tramp_filter_mask
    and x10, x10, x9
.Lfilter_choose:
    cmp x8, #1
    b.eq .Lfilter_eq
    cmp x8, #2
    b.eq .Lfilter_ne
    cmp x8, #3
    b.eq .Lfilter_eq
    cmp x8, #4
    b.eq .Lfilter_lt
    cmp x8, #5
    b.eq .Lfilter_le
    cmp x8, #6
    b.eq .Lfilter_gt
    cmp x8, #7
    b.ne .Lentry_restore
    cmp x10, x11
    b.lo .Lentry_restore
    b .Lfilter_pass
.Lfilter_eq:
    cmp x10, x11
    b.ne .Lentry_restore
    b .Lfilter_pass
.Lfilter_ne:
    cmp x10, x11
    b.eq .Lentry_restore
    b .Lfilter_pass
.Lfilter_lt:
    cmp x10, x11
    b.hs .Lentry_restore
    b .Lfilter_pass
.Lfilter_le:
    cmp x10, x11
    b.hi .Lentry_restore
    b .Lfilter_pass
.Lfilter_gt:
    cmp x10, x11
    b.ls .Lentry_restore
.Lfilter_pass:
    ldr x16, ludp_tramp_call_fn
    cbz x16, .Lafter_call
    adr x12, ludp_tramp_call_active
    atomic_add x12, 1
    /* 512 bytes SIMD, 16 bytes FP status/control, 64 bytes callback context. */
    sub sp, sp, #592
    stp q0, q1, [sp, #0]
    stp q2, q3, [sp, #32]
    stp q4, q5, [sp, #64]
    stp q6, q7, [sp, #96]
    stp q8, q9, [sp, #128]
    stp q10, q11, [sp, #160]
    stp q12, q13, [sp, #192]
    stp q14, q15, [sp, #224]
    stp q16, q17, [sp, #256]
    stp q18, q19, [sp, #288]
    stp q20, q21, [sp, #320]
    stp q22, q23, [sp, #352]
    stp q24, q25, [sp, #384]
    stp q26, q27, [sp, #416]
    stp q28, q29, [sp, #448]
    stp q30, q31, [sp, #480]
    mrs x8, fpcr
    mrs x9, fpsr
    str x8, [sp, #512]
    str x9, [sp, #520]
    add x0, sp, #528
    ldr x8, ludp_tramp_probe_id
    str x8, [x0, #0]
    add x9, sp, #592
    ldp x10, x11, [x9, #0]
    stp x10, x11, [x0, #8]
    ldp x10, x11, [x9, #16]
    stp x10, x11, [x0, #24]
    ldp x10, x11, [x9, #32]
    stp x10, x11, [x0, #40]
    ldr x8, ludp_tramp_call_data
    str x8, [x0, #56]
    blr x16
    ldp q0, q1, [sp, #0]
    ldp q2, q3, [sp, #32]
    ldp q4, q5, [sp, #64]
    ldp q6, q7, [sp, #96]
    ldp q8, q9, [sp, #128]
    ldp q10, q11, [sp, #160]
    ldp q12, q13, [sp, #192]
    ldp q14, q15, [sp, #224]
    ldp q16, q17, [sp, #256]
    ldp q18, q19, [sp, #288]
    ldp q20, q21, [sp, #320]
    ldp q22, q23, [sp, #352]
    ldp q24, q25, [sp, #384]
    ldp q26, q27, [sp, #416]
    ldp q28, q29, [sp, #448]
    ldp q30, q31, [sp, #480]
    ldr x8, [sp, #512]
    ldr x9, [sp, #520]
    msr fpcr, x8
    msr fpsr, x9
    add sp, sp, #592
    adr x12, ludp_tramp_call_active
    atomic_add x12, -1
.Lafter_call:
    ldr x20, ludp_tramp_ring_ptr
    mov x21, xzr
    ldr x8, ludp_tramp_flags
    tbz x8, #1, .Lentry_event
    add x10, sp, #256
    ubfx x10, x10, #4, #12
    mov x11, #4096
    ldr x16, ludp_tramp_slots
.Lslot_search:
    add x17, x16, x10, lsl #8
    add x12, x17, #64
    ldxr x13, [x12]
    cbnz x13, .Lslot_busy
    mov x14, #1
    stxr w15, x14, [x12]
    cbnz w15, .Lslot_search
    mov x21, x17
    b .Lslot_found
.Lslot_busy:
    clrex
    add x10, x10, #1
    and x10, x10, #4095
    subs x11, x11, #1
    b.ne .Lslot_search
    add x12, x20, #168
    atomic_add x12, 1
    b .Lentry_event
.Lslot_found:
    ldr x13, [sp, #240]
    ldr x14, ludp_tramp_probe_id
    stp x13, x14, [x21, #72]
    ldp x13, x14, [sp, #0]
    stp x13, x14, [x21, #88]
    ldp x13, x14, [sp, #16]
    stp x13, x14, [x21, #104]
    ldp x13, x14, [sp, #32]
    stp x13, x14, [x21, #120]
    add x12, x20, #176
    atomic_add x12, 1
    str x13, [x21, #136]
    adr x13, .Lreturn_handler
    str x13, [x21, #152]
    str x21, [sp, #240]
.Lentry_event:
    reserve .Lentry_restore
    isb
    mrs x13, cntvct_el0
    str x13, [x10, #0]
ludp_tramp_rdfsbase:
    mrs x13, tpidr_el0
    str x13, [x10, #8]
    ldr x13, ludp_tramp_probe_id
    str x13, [x10, #16]
    ldp x13, x14, [sp, #0]
    stp x13, x14, [x10, #24]
    ldp x13, x14, [sp, #16]
    stp x13, x14, [x10, #40]
    ldp x13, x14, [sp, #32]
    stp x13, x14, [x10, #56]
    ldr x13, [sp, #240]
    str xzr, [x10, #104]
    cbz x21, .Lentry_caller
    ldr x13, [x21, #136]
    str x13, [x10, #104]
    ldr x13, [x21, #72]
.Lentry_caller:
    str x13, [x10, #72]
    mov x13, #1
    str x13, [x10, #80]
    stp xzr, xzr, [x10, #88]
    stp xzr, xzr, [x10, #112]
    publish
.Lentry_restore:
    restore_gprs
ludp_tramp_reloc_begin:
    .rept 8
    nop
    .endr
ludp_tramp_reloc_end:
ludp_tramp_exit:
    b . /* Replaced by a direct B to continuation/next trampoline. */

/* The unique stub has pushed original x16/x17 and put its address in x16. */
.Lreturn_handler:
    .inst 0xd503249f /* BTI J: valid on Armv8.0 as a NOP hint. */
    save_gprs
    mov x21, x16
    ldp x13, x14, [sp, #256]
    stp x13, x14, [sp, #128]
    ldr x20, ludp_tramp_ring_ptr
    reserve .Lreturn_restore
    isb
    mrs x13, cntvct_el0
    str x13, [x10, #0]
ludp_tramp_return_rdfsbase:
    mrs x13, tpidr_el0
    str x13, [x10, #8]
    ldr x13, [x21, #80]
    str x13, [x10, #16]
    ldp x13, x14, [x21, #88]
    stp x13, x14, [x10, #24]
    ldp x13, x14, [x21, #104]
    stp x13, x14, [x10, #40]
    ldp x13, x14, [x21, #120]
    stp x13, x14, [x10, #56]
    ldr x13, [x21, #72]
    str x13, [x10, #72]
    mov x13, #2
    str x13, [x10, #80]
    ldp x13, x14, [sp, #0]
    stp x13, x14, [x10, #88]
    ldr x13, [x21, #136]
    str x13, [x10, #104]
    str q0, [x10, #112]
    publish
.Lreturn_restore:
    ldr x13, [x21, #72]
    str x13, [sp, #240]
    add x12, x21, #64
    stlr xzr, [x12]
    restore_gprs
    add sp, sp, #16
    ret
ludp_tramp_code_end:
.p2align 3
ludp_tramp_data_begin:
ludp_tramp_ret_addr: .quad 0
ludp_tramp_ring_ptr: .quad 0
ludp_tramp_probe_id: .quad 0
ludp_tramp_slots: .quad 0
ludp_tramp_flags: .quad 0
ludp_tramp_filter_op: .quad 0
ludp_tramp_filter_arg: .quad 0
ludp_tramp_filter_value: .quad 0
ludp_tramp_filter_mask: .quad 0
ludp_tramp_call_fn: .quad 0
ludp_tramp_call_data: .quad 0
ludp_tramp_call_active: .quad 0
/* ABI-compatible placeholders; ARM does not use XSAVE. */
ludp_tramp_xsave_mode: .quad 0
ludp_tramp_xsave_size: .quad 0
ludp_tramp_xsave_mask: .quad 0
ludp_tramp_data_end:
ludp_tramp_end:

.p2align 4
ludp_return_stub:
    stp x16, x17, [sp, #-16]!
    adr x16, ludp_return_stub
    ldr x17, [x16, #152]
    br x17
    .rept 12
    nop
    .endr
ludp_return_stub_end:
.section .note.GNU-stack,"",@progbits
