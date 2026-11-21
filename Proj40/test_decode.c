#include <stdio.h>
#include <string.h>
#include "ludp_internal.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)
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
