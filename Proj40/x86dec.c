/*
 * x86dec.c - compact x86-64 instruction length + operand decoder.
 *
 * Only needs to be good enough to (a) find instruction boundaries when
 * overwriting a function entry with a 5-byte jmp, and (b) locate RIP-relative
 * and relative-branch operands so they can be relocated into a trampoline.
 * Returns 0 for anything it cannot safely decode.
 */
#include <stdint.h>
#include <stddef.h>
#include "ludp_internal.h"

/* Scan the ModRM (+SIB + displacement) part.
 * p points at the ModRM byte. Returns bytes consumed (modrm+sib+disp).
 * Sets *rip = 1 and *disp32_off = offset (from p) of the disp32 when the
 * operand is RIP-relative. */
static int modrm_scan(const uint8_t *p, int maxlen, int *rip, int *disp32_off)
{
    if (maxlen < 1)
        return 0;
    uint8_t modrm = p[0];
    int mod = modrm >> 6, rm = modrm & 7;
    int sib = 0, disp = 0, d32 = -1, is_rip = 0;

    if (mod != 3 && rm == 4) {           /* SIB follows */
        if (maxlen < 2)
            return 0;
        sib = 1;
        uint8_t s = p[1];
        if (mod == 0 && (s & 7) == 5) {  /* SIB with no base is absolute */
            disp = 4;
        }
    } else if (mod == 0 && rm == 5) {    /* RIP-relative disp32 */
        disp = 4; is_rip = 1; d32 = 1;
    }
    if (mod == 1) {
        disp = 1;
    } else if (mod == 2) {
        disp = 4;
    }
    if (rip) *rip = is_rip;
    if (disp32_off) *disp32_off = d32;
    return 1 + sib + disp <= maxlen ? 1 + sib + disp : 0;
}

int x86_dec_one(const uint8_t *p, int maxlen, x86_insn *out)
{
    out->len = 0; out->rip_off = -1; out->rel_off = -1; out->rel_size = 0;
    out->terminates = false;
    if (maxlen <= 0)
        return 0;

    int i = 0, rex = 0, rex_w = 0, saw66 = 0;

    /* legacy / mandatory prefixes */
    while (i < maxlen) {
        uint8_t b = p[i];
        if (b == 0x66) { saw66 = 1; i++; continue; }
        if (b == 0x67) return 0; /* address-size override: fail closed */
        if (b == 0xF0 || b == 0xF2 || b == 0xF3) { i++; continue; }
        if (b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 ||
            b == 0x64 || b == 0x65) { i++; continue; }
        break;
    }
    if (i < maxlen && (p[i] & 0xF0) == 0x40 && p[i] <= 0x4F) {
        rex = p[i]; rex_w = (rex >> 3) & 1; i++;
    }
    if (i >= maxlen)
        return 0;

    uint8_t op = p[i];
    int len = 0;
    out->terminates = op == 0xc2 || op == 0xc3 || op == 0xca || op == 0xcb ||
                      op == 0xcf || op == 0xe9 || op == 0xeb;
    if (op == 0xff && i + 1 < maxlen) {
        int group = (p[i + 1] >> 3) & 7;
        out->terminates = group == 4 || group == 5;
    }

    /* ---------------- two-byte 0F opcodes ---------------- */
    if (op == 0x0F) {
        if (i + 1 >= maxlen)
            return 0;
        uint8_t op2 = p[i + 1];
        /* Unsupported opcode maps must never fall through as generic ModRM. */
        if (op2 == 0x0f || op2 == 0x38 || op2 == 0x3a) return 0;

        /* no-ModRM 0F opcodes */
        switch (op2) {
        case 0x05: case 0x06: case 0x07:
        case 0x0B: case 0x0E:
        case 0x30: case 0x31: case 0x32:
        case 0x34: case 0x35:
        case 0x77:
        case 0xA0: case 0xA1: case 0xA2:
        case 0xA8: case 0xA9: case 0xAA:
        case 0xC8: case 0xC9: case 0xCA: case 0xCB:
        case 0xCC: case 0xCD: case 0xCE: case 0xCF:
            len = 2;
            goto done;
        default:
            break;
        }
        /* jcc rel32 (0F 80 .. 0F 8F) */
        if ((op2 & 0xF0) == 0x80) {
            out->rel_off = i + 2; out->rel_size = 4;
            len = 6;
            goto done;
        }
        /* endbr64 / endbr32 (0F 1E FA / FB) */
        if (op2 == 0x1E && i + 2 < maxlen &&
            (p[i + 2] == 0xFA || p[i + 2] == 0xFB)) {
            len = 3;
            goto done;
        }
        /* ModRM-based two-byte opcodes */
        if (!((op2 >= 0x10 && op2 <= 0x18) || op2 == 0x1e || op2 == 0x1f ||
              (op2 >= 0x28 && op2 <= 0x2f) || (op2 >= 0x40 && op2 <= 0x76) ||
              op2 == 0x7e || op2 == 0x7f || (op2 >= 0x90 && op2 <= 0x9f) ||
              op2 == 0xa3 || op2 == 0xa4 || op2 == 0xa5 ||
              (op2 >= 0xab && op2 <= 0xaf) || (op2 >= 0xb0 && op2 <= 0xc7) ||
              op2 >= 0xd0)) return 0;
        if (i + 2 >= maxlen)
            return 0;
        {
            int rip, d32;
            int mr = modrm_scan(p + i + 2, maxlen - (i + 2), &rip, &d32);
            if (mr == 0)
                return 0;
            int imm = 0;
            if (op2 == 0x38 || op2 == 0x3A) return 0;
            if (op2 == 0xBA || op2 == 0x70 ||
                (op2 >= 0x71 && op2 <= 0x73) || op2 == 0xC2 ||
                op2 == 0xC4 || op2 == 0xC5 || op2 == 0xC6 ||
                op2 == 0xA4 || op2 == 0xAC)
                imm = 1; /* imm8 */
            len = 2 + mr + imm;
            if (rip)
                out->rip_off = i + 2 + d32;
        }
        goto done;
    }

    /* ---------------- one-byte, no-ModRM opcodes ---------------- */
    if (op >= 0x50 && op <= 0x5F) { len = 1; goto done; }        /* push/pop r */
    if (op >= 0x90 && op <= 0x97) { len = 1; goto done; }        /* nop / xchg eax */
    if (op >= 0x98 && op <= 0x9F && op != 0x9A) { len = 1; goto done; }
    if (op >= 0x70 && op <= 0x7F) { out->rel_off = i + 1; out->rel_size = 1; len = 2; goto done; }
    if (op >= 0xB0 && op <= 0xB7) { len = 2; goto done; }        /* mov r8, imm8 */
    if (op >= 0xB8 && op <= 0xBF) { len = rex_w ? 9 : (saw66 ? 3 : 5); goto done; }
    if (op <= 0x3d && (op & 7) == 4) { len = 2; goto done; }
    if (op <= 0x3d && (op & 7) == 5) { len = saw66 && !rex_w ? 3 : 5; goto done; }
    if (op == 0xA8) { len = 2; goto done; }
    if (op == 0xA9) { len = saw66 && !rex_w ? 3 : 5; goto done; }
    if (op == 0xC2) { len = 3; goto done; }
    if (op == 0xC3) { len = 1; goto done; }
    if (op == 0xC8) { len = 4; goto done; }
    if (op == 0xC9) { len = 1; goto done; }
    if (op == 0xCA) { len = 3; goto done; }
    if (op == 0xCB || op == 0xCC || op == 0xCE) { len = 1; goto done; }
    if (op == 0xCD) { len = 2; goto done; }
    if (op == 0xCF) { len = 1; goto done; }
    if (op >= 0xE0 && op <= 0xE3) { out->rel_off = i + 1; out->rel_size = 1; len = 2; goto done; }
    if (op == 0xE8 || op == 0xE9) { out->rel_off = i + 1; out->rel_size = 4; len = 5; goto done; }
    if (op == 0xEB) { out->rel_off = i + 1; out->rel_size = 1; len = 2; goto done; }
    if (op == 0x68) { len = saw66 ? 3 : 5; goto done; }          /* push imm */
    if (op == 0x6A) { len = 2; goto done; }                      /* push imm8 */
    if (op == 0xA0 || op == 0xA1 || op == 0xA2 || op == 0xA3) { len = 9; goto done; }
    if (op == 0x6C || op == 0x6D || op == 0x6E || op == 0x6F) { len = 1; goto done; }
    if (op >= 0xA4 && op <= 0xA7) { len = 1; goto done; }
    if (op >= 0xAA && op <= 0xAF) { len = 1; goto done; }
    if (op >= 0xE4 && op <= 0xE7) { len = 2; goto done; }
    if (op >= 0xEC && op <= 0xEF) { len = 1; goto done; }
    if (op == 0xF4 || op == 0xF5 || op == 0xF8 || op == 0xF9 ||
        op == 0xFA || op == 0xFB || op == 0xFC || op == 0xFD) { len = 1; goto done; }
    if (op == 0xD7) { len = 1; goto done; }                      /* xlat */
    if (op == 0xC4 || op == 0xC5 || op == 0x62) return 0; /* VEX/EVEX */

    if (!((op <= 0x3b && (op & 7) <= 3) || op == 0x63 ||
          op == 0x69 || op == 0x6b || (op >= 0x80 && op <= 0x8f) ||
          op == 0xc0 || op == 0xc1 || op == 0xc6 || op == 0xc7 ||
          (op >= 0xd0 && op <= 0xd3) || (op >= 0xd8 && op <= 0xdf) ||
          op == 0xf6 || op == 0xf7 || op == 0xfe || op == 0xff)) return 0;

    /* ---------------- ModRM one-byte opcodes ---------------- */
    if (i + 1 >= maxlen)
        return 0;
    if (op == 0xc7 && p[i + 1] == 0xf8) return 0; /* XBEGIN is PC-relative */
    {
        int rip, d32;
        int mr = modrm_scan(p + i + 1, maxlen - (i + 1), &rip, &d32);
        if (mr == 0)
            return 0;
        int imm = 0;
        switch (op) {
        case 0x80: case 0x82: case 0x83: imm = 1; break;
        case 0x81: imm = saw66 && !rex_w ? 2 : 4; break;
        case 0xC0: case 0xC1: imm = 1; break;
        case 0xC6: imm = 1; break;
        case 0xC7: imm = saw66 && !rex_w ? 2 : 4; break;
        case 0x69: imm = saw66 && !rex_w ? 2 : 4; break;
        case 0x6B: imm = 1; break;
        case 0xF6: if (((p[i + 1] >> 3) & 7) <= 1) imm = 1; break;
        case 0xF7: if (((p[i + 1] >> 3) & 7) <= 1) imm = (saw66 && !rex_w ? 2 : 4); break;
        default: imm = 0; break;
        }
        len = 1 + mr + imm;
        if (rip)
            out->rip_off = i + 1 + d32;
    }

done:
    if (i + len > maxlen || i + len > 15) return 0;
    out->len = i + len;
    return out->len;
}
