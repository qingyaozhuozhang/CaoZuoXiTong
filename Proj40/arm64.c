/* Native Linux AArch64 instruction and ptrace backend. */
#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <asm/ptrace.h>
#include "ludp_internal.h"

#ifndef NT_ARM_GCS
#define NT_ARM_GCS 0x410
#endif
#define A64_NOP UINT32_C(0xd503201f)
#define A64_SVC UINT32_C(0xd4000001)
#define A64_BRANCH_RANGE (INT64_C(1) << 27)

static int encode_branch(uintptr_t from, uintptr_t to, uint32_t *op)
{
    int64_t diff = (int64_t)to - (int64_t)from;
    if ((diff & 3) || diff < -A64_BRANCH_RANGE || diff >= A64_BRANCH_RANGE) {
        errno = ERANGE;
        return -1;
    }
    *op = UINT32_C(0x14000000) | ((uint32_t)(diff / 4) & UINT32_C(0x03ffffff));
    return 0;
}

bool arch_branch_reaches(uintptr_t from, uintptr_t to)
{
    /* Leave room for the branch back from the end of the trampoline. */
    int64_t diff = (int64_t)to - (int64_t)from;
    return !(diff & 3) && diff > -A64_BRANCH_RANGE + 4096 &&
           diff < A64_BRANCH_RANGE - 4096;
}

void arch_fill_nops(uint8_t *bytes, size_t len)
{
    const uint32_t nop = A64_NOP;
    for (size_t i = 0; i + sizeof nop <= len; i += sizeof nop)
        memcpy(bytes + i, &nop, sizeof nop);
}

int arch_read_entry(ludp_ctx *c, uintptr_t addr, uint8_t orig[32])
{
    if (mem_read(c, addr, orig, 4)) return -1;
    uint8_t scratch[16];
    return relocate_orig(orig, 4, addr, addr, scratch, sizeof scratch) < 0 ? -1 : 4;
}

int arch_write_patch(ludp_ctx *c, uintptr_t addr, uintptr_t target,
                     const uint8_t *orig, int len)
{
    uint32_t branch;
    if (!target) return mem_write_code(c, addr, orig, (size_t)len);
    if (encode_branch(addr, target, &branch)) return -1;
    return mem_write_code(c, addr, &branch, sizeof branch);
}

int arch_link_trampoline(ludp_ctx *c, uintptr_t tramp, uintptr_t next)
{
    uintptr_t pc = tramp + tramp_layout()->off_ret;
    uint32_t branch;
    if (encode_branch(pc, next, &branch)) return -1;
    return mem_write_code(c, pc, &branch, sizeof branch);
}

int relocate_orig(const uint8_t *orig, int orig_len, uintptr_t orig_addr,
                  uintptr_t new_addr, uint8_t *out, int out_cap)
{
    (void)new_addr;
    if (orig_len != 4 || out_cap < 4) { errno = EINVAL; return -1; }

    uint32_t op;
    memcpy(&op, orig, sizeof op);
    /* ADR and ADRP are replaced by a MOVZ/MOVK sequence with the original
     * absolute value. It modifies only the destination register. */
    if ((op & UINT32_C(0x1f000000)) == UINT32_C(0x10000000)) {
        if (out_cap < 16) { errno = ENOSPC; return -1; }
        uint32_t imm = ((op >> 5) & UINT32_C(0x7ffff)) << 2 | ((op >> 29) & 3);
        int64_t offset = (int64_t)(imm ^ UINT32_C(0x100000)) - INT64_C(0x100000);
        uint64_t value = orig_addr + offset;
        if (op >> 31) value = (orig_addr & ~(uintptr_t)4095) + offset * 4096;
        for (unsigned i = 0; i < 4; ++i) {
            uint32_t move = (i ? UINT32_C(0xf2800000) : UINT32_C(0xd2800000)) |
                            (i << 21) | (((value >> (i * 16)) & 0xffff) << 5) |
                            (op & 31);
            memcpy(out + i * sizeof move, &move, sizeof move);
        }
        return 16;
    }

    unsigned major = (op >> 25) & 15;
    bool arithmetic = major == 5 || major == 8 || major == 9 || major == 13;
    bool load_store = (major & 5) == 4;
    bool literal = (op & UINT32_C(0x3b000000)) == UINT32_C(0x18000000);
    /* Treat branches, exception/system instructions, and literal loads as
     * unsupported at an entry. NOP is the portable patchable-entry marker. */
    if (!arithmetic && !(load_store && !literal) && op != A64_NOP) {
        errno = ENOTSUP;
        return -1;
    }
    memcpy(out, orig, sizeof op);
    return sizeof op;
}

static int regset(pid_t tid, int note, void *buf, size_t len, bool write)
{
    struct iovec io = { buf, len };
    if (ptrace(write ? PTRACE_SETREGSET : PTRACE_GETREGSET, tid,
               (void *)(uintptr_t)note, &io) < 0)
        return -1;
    if (io.iov_len != len) { errno = ENOTSUP; return -1; }
    return 0;
}

int arm64_thread_state(pid_t tid, uintptr_t *pc, bool *in_syscall)
{
    struct user_pt_regs regs;
    int syscall;
    if (regset(tid, NT_PRSTATUS, &regs, sizeof regs, false) ||
        regset(tid, NT_ARM_SYSTEM_CALL, &syscall, sizeof syscall, false))
        return -1;
    *pc = regs.pc;
    *in_syscall = syscall != -1;
    return 0;
}

uintptr_t arm64_find_svc(ludp_ctx *c)
{
    char path[64], line[1024];
    snprintf(path, sizeof path, "/proc/%d/maps", c->pid);
    FILE *maps = fopen(path, "r");
    if (!maps) return 0;
    uintptr_t found = 0;
    /* A libc wrapper is a fallback for kernels with a vDSO without SVC. */
    for (int pass = 0; pass < 2 && !found; ++pass) {
        rewind(maps);
        while (!found && fgets(line, sizeof line, maps)) {
            uintptr_t lo, hi;
            char perm[5], name[512] = {0};
            if (sscanf(line, "%lx-%lx %4s %*s %*s %*s %511s",
                       &lo, &hi, perm, name) != 4 || perm[2] != 'x')
                continue;
            const char *base = strrchr(name, '/');
            base = base ? base + 1 : name;
            bool is_vdso = !strcmp(base, "[vdso]");
            bool is_libc = !strncmp(base, "libc.so", 7) ||
                           !strncmp(base, "libc-", 5) ||
                           !strncmp(base, "ld-musl-", 8);
            if (!(pass == 0 ? is_vdso : is_libc)) continue;
            uint32_t words[1024];
            for (uintptr_t p = lo; p < hi && !found; ) {
                size_t bytes = hi - p < sizeof words ? hi - p : sizeof words;
                if (mem_read(c, p, words, bytes)) break;
                for (size_t i = 0; i < bytes / 4; ++i)
                    if (words[i] == A64_SVC) { found = p + i * 4; break; }
                p += bytes;
            }
        }
    }
    fclose(maps);
    if (!found) errno = ENOTSUP;
    return found;
}

static int syscall_stop(pid_t tid)
{
    if (ptrace(PTRACE_SYSCALL, tid, 0, 0)) return -1;
    int status = wait_stop(tid, 3000);
    if (status >= 0 && WIFSTOPPED(status) && WSTOPSIG(status) == (SIGTRAP | 0x80) &&
        !(status >> 16))
        return 0;
    errno = status < 0 ? ETIMEDOUT : EINTR;
    return -1;
}

int arm64_syscall(ludp_ctx *c, long nr, const unsigned long args[6], uintptr_t *ret)
{
    struct user_pt_regs orig, regs;
    pid_t tid = -1;
    const int no_syscall = -1;
    for (int i = 0; i < c->nthreads; ++i) {
        int syscall;
        pid_t candidate = c->tids[i];
        if (!c->seized[i] ||
            regset(candidate, NT_ARM_SYSTEM_CALL, &syscall, sizeof syscall, false) ||
            syscall != no_syscall ||
            regset(candidate, NT_PRSTATUS, &orig, sizeof orig, false))
            continue;
        tid = candidate;
        break;
    }
    if (tid < 0) { errno = EBUSY; return -1; }

    /* Keep asynchronous signals pending while the scratch register state is
     * active. Linux exposes a 64-bit kernel signal set via ptrace. */
    uint64_t mask, blocked = UINT64_MAX;
    if (ptrace(PTRACE_GETSIGMASK, tid, sizeof mask, &mask) ||
        ptrace(PTRACE_SETSIGMASK, tid, sizeof blocked, &blocked))
        return -1;

    regs = orig;
    memcpy(regs.regs, args, 6 * sizeof *args);
    regs.regs[8] = nr;
    regs.pc = c->syscall_insn;
    int rc = regset(tid, NT_PRSTATUS, &regs, sizeof regs, true);
    if (!rc) rc = syscall_stop(tid);
    if (!rc) rc = syscall_stop(tid);
    if (!rc) {
        rc = regset(tid, NT_PRSTATUS, &regs, sizeof regs, false);
        if (!rc && ret) *ret = regs.regs[0];
    }
    int failure = rc ? errno : 0;
    if (regset(tid, NT_PRSTATUS, &orig, sizeof orig, true)) failure = errno;
    int reset = no_syscall;
    if (regset(tid, NT_ARM_SYSTEM_CALL, &reset, sizeof reset, true)) failure = errno;
    if (ptrace(PTRACE_SETSIGMASK, tid, sizeof mask, &mask)) failure = errno;
    if (failure) { errno = failure; return -1; }
    return 0;
}

int arm64_check_return_abi(ludp_ctx *c)
{
    /* An active guarded control stack authenticates the return address.
     * Kernels without that regset return EINVAL/EIO. */
    for (int i = 0; i < c->nthreads; ++i) {
        if (!c->seized[i]) continue;
        uint64_t gcs[3];
        if (!regset(c->tids[i], NT_ARM_GCS, gcs, sizeof gcs, false)) {
            if (gcs[0] & 1) { errno = ENOTSUP; return -1; }
        } else if (errno != EINVAL && errno != EIO && errno != ENODEV) {
            return -1;
        }
    }
    return 0;
}
