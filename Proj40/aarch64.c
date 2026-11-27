/* AArch64 fixed-width entry decoding and relocation.
 * Instructions are accepted only when their PC semantics are understood.
 * ADR/ADRP expand into MOVZ/MOVK without borrowing a scratch register. */
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include "ludp_internal.h"

static int64_t signed_bits(uint32_t value, unsigned bits)
{
    uint32_t sign = UINT32_C(1) << (bits - 1);
    return (int64_t)(value ^ sign) - sign;
}

int aarch64_branch(uintptr_t from, uintptr_t to, uint32_t *out)
{
    if (!out) { errno = EINVAL; return -1; }
    int64_t delta = (int64_t)to - (int64_t)from;
    if ((from & 3) || (to & 3) || delta < -(INT64_C(1) << 27) ||
        delta >= (INT64_C(1) << 27)) {
        errno = ERANGE;
        return -1;
    }
    *out = UINT32_C(0x14000000) | ((uint32_t)(delta / 4) & 0x03ffffffu);
    return 0;
}

static bool supported(uint32_t op)
{
    /* ADR/ADRP; unconditional B (BL requires different LR semantics). */
    if ((op & 0x1f000000u) == 0x10000000u ||
        (op & 0xfc000000u) == 0x14000000u)
        return true;
    /* B.cond, CBZ/CBNZ and TBZ/TBNZ. */
    if ((op & 0xff000010u) == 0x54000000u ||
        (op & 0x7e000000u) == 0x34000000u ||
        (op & 0x7e000000u) == 0x36000000u)
        return true;
    /* RET Xn remains valid at its new address; the return probe redirects LR. */
    if ((op & 0xfffffc1fu) == 0xd65f0000u || op == 0xd503201fu)
        return true;
    unsigned major = (op >> 25) & 15;
    /* Data processing immediate/register and FP/SIMD, including literal
     * loads (handled separately below). Exclude reserved/branch/system/SVE
     * groups. No accepted ordinary instruction computes a PC implicitly. */
    return major == 5 || major == 8 || major == 9 || major == 13 ||
           major == 7 || major == 15 || (major & 5) == 4;
}

int aarch64_decode_prefix(const uint8_t *bytes, int maxlen, int *len)
{
    if (!bytes || !len || maxlen < 4) { errno = EINVAL; return -1; }
    uint32_t op;
    memcpy(&op, bytes, sizeof op);
    if (!supported(op)) { errno = ENOTSUP; return -1; }
    *len = 4;
    return 0;
}

int relocate_orig(const uint8_t *orig, int orig_len, uintptr_t orig_addr,
                  uintptr_t new_addr, uint8_t *out, int out_cap)
{
    if (!orig || !out || orig_len != 4 || out_cap < 4 ||
        (orig_addr & 3) || (new_addr & 3)) {
        errno = EINVAL;
        return -1;
    }
    uint32_t op;
    memcpy(&op, orig, sizeof op);
    if (!supported(op)) { errno = ENOTSUP; return -1; }
    if ((op & 0x1f000000u) == 0x10000000u) {
        if (out_cap < 16) { errno = ENOSPC; return -1; }
        uint32_t immediate = (((op >> 5) & 0x7ffffu) << 2) | ((op >> 29) & 3);
        int64_t displacement = signed_bits(immediate, 21);
        uint64_t address = orig_addr + displacement;
        if (op >> 31)
            address = (orig_addr & ~(uintptr_t)4095) + displacement * 4096;
        for (unsigned half = 0; half < 4; ++half) {
            uint32_t move = (half ? 0xf2800000u : 0xd2800000u) |
                            (half << 21) |
                            ((uint32_t)((address >> (half * 16)) & 0xffff) << 5) |
                            (op & 31);
            memcpy(out + half * 4, &move, sizeof move);
        }
        return 16;
    }
    unsigned width = 0, shift = 0;
    if ((op & 0xfc000000u) == 0x14000000u)
        width = 26;
    else if ((op & 0xff000010u) == 0x54000000u ||
             (op & 0x7e000000u) == 0x34000000u ||
             (op & 0x3b000000u) == 0x18000000u) {
        width = 19;
        shift = 5;
    } else if ((op & 0x7e000000u) == 0x36000000u) {
        width = 14;
        shift = 5;
    }
    if (width) {
        uint32_t mask = (UINT32_C(1) << width) - 1;
        uintptr_t target = orig_addr + signed_bits((op >> shift) & mask, width) * 4;
        bool literal = (op & 0x3b000000u) == 0x18000000u;
        /* Branches into the displaced instruction must still reach code;
         * literal loads keep referring to their original data address. */
        if (!literal && target == orig_addr)
            target = new_addr;
        int64_t displacement = (int64_t)target - (int64_t)new_addr;
        int64_t limit = INT64_C(1) << (width + 1);
        if ((displacement & 3) || displacement < -limit || displacement >= limit) {
            errno = ERANGE;
            return -1;
        }
        op = (op & ~(mask << shift)) |
             (((uint32_t)(displacement / 4) & mask) << shift);
    }
    memcpy(out, &op, sizeof op);
    return 4;
}
