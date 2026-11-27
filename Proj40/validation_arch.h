#ifndef LUDP_VALIDATION_ARCH_H
#define LUDP_VALIDATION_ARCH_H

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#if defined(__x86_64__)
#define VALIDATION_PATCH_SIZE 5
#define VALIDATION_ENTRY_READ_SIZE 5
#define VALIDATION_BRANCH_NAME "JMP rel32"
#define VALIDATION_INTEGER_REGISTER "RAX"
#define VALIDATION_VECTOR_REGISTER "XMM0"
#define VALIDATION_ABI_SNAPSHOT_SIZE 672
#elif defined(__aarch64__)
#define VALIDATION_PATCH_SIZE 4
#define VALIDATION_ENTRY_READ_SIZE 8
#define VALIDATION_BRANCH_NAME "B imm26"
#define VALIDATION_INTEGER_REGISTER "X0"
#define VALIDATION_VECTOR_REGISTER "V0"
#define VALIDATION_ABI_SNAPSHOT_SIZE 800
#else
#error "Validation supports Linux x86_64 and AArch64"
#endif

/* These checks trace a forked child with the same libc mapping. Keep the
 * symbol-based install path and use the actual object name on each libc. */
static inline int validation_symbol_object(const void *address, char path[PATH_MAX])
{
    Dl_info object;
    if (!address || !dladdr(address, &object) || !object.dli_fname) {
        errno = ENOENT;
        return -1;
    }
    return realpath(object.dli_fname, path) ? 0 : -1;
}

static inline void validation_cpu_relax(void)
{
#if defined(__x86_64__)
    __asm__ volatile("pause");
#else
    __asm__ volatile("yield");
#endif
}

static inline bool validation_entry_is_branch(const unsigned char *bytes)
{
#if defined(__x86_64__)
    return bytes[0] == 0xe9;
#else
    uint32_t insn;
    memcpy(&insn, bytes, sizeof insn);
    if ((insn & UINT32_C(0xffffff3f)) == UINT32_C(0xd503241f))
        memcpy(&insn, bytes + 4, sizeof insn);
    return (insn & UINT32_C(0xfc000000)) == UINT32_C(0x14000000);
#endif
}
#endif
