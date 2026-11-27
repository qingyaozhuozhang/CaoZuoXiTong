#include <stdio.h>
#include <string.h>
#include "ludp_internal.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)
#if defined(__x86_64__)
int main(void)
{
    x86_insn d;
    const unsigned char sib8[] = {0x48,0x8b,0x44,0x24,0x08};
    CHECK(x86_dec_one(sib8, sizeof sib8, &d) == 5);
    const unsigned char sib32[] = {0x48,0x8b,0x84,0x24,0x00,0x01,0x00,0x00};
    CHECK(x86_dec_one(sib32, sizeof sib32, &d) == 8);
    const unsigned char absolute[] = {0x48,0x8b,0x04,0x25,0x00,0x10,0x00,0x00};
    CHECK(x86_dec_one(absolute, sizeof absolute, &d) == 8 && d.rip_off == -1);
    const unsigned char rip[] = {0x48,0x8b,0x05,0x00,0x10,0x00,0x00};
    CHECK(x86_dec_one(rip, sizeof rip, &d) == 7 && d.rip_off == 3);
    for (int n = 1; n < 8; n++) CHECK(x86_dec_one(sib32, n, &d) == 0);
    const unsigned char evex[] = {0x62,0xf1,0x7c,0x48,0x58,0xc0};
    CHECK(x86_dec_one(evex, sizeof evex, &d) == 0);
    const unsigned char acc[] = {0x48,0x05,0x01,0x00,0x00,0x00};
    CHECK(x86_dec_one(acc, sizeof acc, &d) == 6);
    unsigned char out[32];
    const unsigned char branch[] = {0xeb,0x02,0x90,0x90,0x90};
    CHECK(relocate_orig(branch, sizeof branch, 0x1000, 0x100000, out, sizeof out) == 5);
    CHECK(out[1] == 2);
    const unsigned char rex66[] = {0x66,0x48,0x81,0xc0,1,0,0,0};
    CHECK(x86_dec_one(rex66,sizeof rex66,&d) == 8);
    const unsigned char threednow[] = {0x0f,0x0f,0xc0,0xbf};
    CHECK(x86_dec_one(threednow,sizeof threednow,&d) == 0);
    const unsigned char xbegin[] = {0xc7,0xf8,0,0,0,0};
    CHECK(x86_dec_one(xbegin,sizeof xbegin,&d) == 0);
    const unsigned char movabs[] = {0x48,0xb8,1,2,3,4,5,6,7,8};
    CHECK(x86_dec_one(movabs,sizeof movabs,&d) == 10);
    if (!failures) puts("decoder PASS");
    return failures != 0;
}

#else
static uint32_t read_instruction(const unsigned char *bytes)
{
    uint32_t op;
    memcpy(&op, bytes, sizeof op);
    return op;
}

static uint64_t decoded_address(const unsigned char *bytes, unsigned rd)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 4; ++i) {
        uint32_t op = read_instruction(bytes + i * 4);
        CHECK((op & 31) == rd);
        CHECK((op & 0xff800000u) == (i ? 0xf2800000u : 0xd2800000u));
        CHECK(((op >> 21) & 3) == i);
        value |= (uint64_t)((op >> 5) & 0xffff) << (i * 16);
    }
    return value;
}

int main(void)
{
    int len = 0;
    uint32_t nop = 0xd503201f, ret = 0xd65f03c0, op = 0;
    unsigned char out[32];
    CHECK(aarch64_decode_prefix((const uint8_t *)&nop, 4, &len) == 0 && len == 4);
    for (int n = 0; n < 4; ++n)
        CHECK(aarch64_decode_prefix((const uint8_t *)&nop, n, &len) == -1);
    CHECK(aarch64_decode_prefix((const uint8_t *)&ret, 4, &len) == 0 && len == 4);
    CHECK(relocate_orig((const uint8_t *)&ret, 4, 0x1000, 0x2000, out, sizeof out) == 4);
    CHECK(read_instruction(out) == ret);
    const uint32_t unsupported[] = {0xd4000001, 0x94000001, 0xd503233f, 0xd61f0000, 0};
    for (unsigned i = 0; i < sizeof unsupported / sizeof *unsupported; ++i)
        CHECK(aarch64_decode_prefix((const uint8_t *)&unsupported[i], 4, &len) == -1);

    CHECK(aarch64_branch(0x10000000, 0x10000004, &op) == 0 && op == 0x14000001);
    CHECK(aarch64_branch(0x10000000, 0x08000000, &op) == 0 && op == 0x16000000);
    CHECK(aarch64_branch(0x10000000, 0x17fffffc, &op) == 0 && op == 0x15ffffff);
    CHECK(aarch64_branch(0x10000000, 0x18000000, &op) == -1);
    CHECK(aarch64_branch(0x10000000, 0x07fffffc, &op) == -1);
    CHECK(aarch64_branch(0x1000, 0x1001, &op) == -1);
    CHECK(aarch64_branch(0x1001, 0x1004, &op) == -1);
    CHECK(aarch64_branch(0x1000, 0x1004, NULL) == -1);

    const uint32_t branches[] = {0x14000004, 0x54000080, 0xb4000080, 0x36000080, 0x58000080};
    for (unsigned i = 0; i < sizeof branches / sizeof *branches; ++i) {
        CHECK(relocate_orig((const uint8_t *)&branches[i], 4, 0x1000, 0x1004, out, sizeof out) == 4);
        uint32_t delta = i ? (1u << 5) : 1;
        CHECK(read_instruction(out) == branches[i] - delta);
    }
    uint32_t loop = 0x14000000;
    CHECK(relocate_orig((const uint8_t *)&loop, 4, 0x1000, 0x2000, out, sizeof out) == 4);
    CHECK(read_instruction(out) == loop);
    uint32_t literal = 0x58000000;
    CHECK(relocate_orig((const uint8_t *)&literal, 4, 0x1000, 0x1004, out, sizeof out) == 4);
    CHECK(read_instruction(out) == 0x58ffffe0);
    uint32_t tbz = 0x36000020;
    CHECK(relocate_orig((const uint8_t *)&tbz, 4, 0x1000, 0x100000, out, sizeof out) == -1);
    uint32_t conditional = 0x54000020;
    CHECK(relocate_orig((const uint8_t *)&conditional, 4, 0x1000, 0x400000, out, sizeof out) == -1);

    uint32_t adr_forward = 0x10000080; /* ADR X0, PC + 16 */
    CHECK(relocate_orig((const uint8_t *)&adr_forward, 4, 0x1000, 0x100000000, out, sizeof out) == 16);
    CHECK(decoded_address(out, 0) == 0x1010);
    uint32_t adr_backward = 0x10ffff83; /* ADR X3, PC - 16 */
    CHECK(relocate_orig((const uint8_t *)&adr_backward, 4, 0x1000, 0x2000, out, sizeof out) == 16);
    CHECK(decoded_address(out, 3) == 0xff0);
    uint32_t adrp_forward = 0xb0000005; /* ADRP X5, page(PC) + 4096 */
    CHECK(relocate_orig((const uint8_t *)&adrp_forward, 4, 0x1234, 0x100000000, out, sizeof out) == 16);
    CHECK(decoded_address(out, 5) == 0x2000);
    uint32_t adrp_backward = 0xf0ffffe6; /* ADRP X6, page(PC) - 4096 */
    CHECK(relocate_orig((const uint8_t *)&adrp_backward, 4, 0x2234, 0x100000000, out, sizeof out) == 16);
    CHECK(decoded_address(out, 6) == 0x1000);
    CHECK(relocate_orig((const uint8_t *)&adr_forward, 4, 0x1000, 0x2000, out, 15) == -1);
    CHECK(relocate_orig((const uint8_t *)&nop, 4, 0x1001, 0x2000, out, sizeof out) == -1);
    if (!failures) puts("AArch64 decoder/relocation PASS");
    return failures != 0;
}
#endif
