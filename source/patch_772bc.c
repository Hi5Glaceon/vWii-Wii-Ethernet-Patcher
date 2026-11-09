// 772B/C patches - register-compatible with 772A, just fix PID + rxctrl + IPOSC

#include <stdio.h>
#include <string.h>
#include <gccore.h>
#include "patch_common.h"
#include "patch_772bc.h"
#include "sha1.h"

// "/dev/usb/ehc/0b95/772"
static const u8 pat_ehc[] = {
    0x2F,0x64,0x65,0x76,0x2F,0x75,0x73,0x62,
    0x2F,0x65,0x68,0x63,0x2F,0x30,0x62,0x39,
    0x35,0x2F,0x37,0x37,0x32
};
// "/dev/usb/oh0/0b95/772"
static const u8 pat_oh0[] = {
    0x2F,0x64,0x65,0x76,0x2F,0x75,0x73,0x62,
    0x2F,0x6F,0x68,0x30,0x2F,0x30,0x62,0x39,
    0x35,0x2F,0x37,0x37,0x32
};

// MOV R3, #0x7700
static const u8 pat_mov_pid[] = { 0xE3, 0xA0, 0x3C, 0x77 };

// rxctrl B - MOV R1, #0x218 + branch
static const u8 pat_rxctrl_b[] = { 0xE3,0xA0,0x1F,0x86, 0xEA,0xFF,0xFF,0x8C };
static const u8 pat_rxctrl_b_old[] = { 0xE3,0xA0,0x1F,0x46, 0xEA,0xFF,0xFF,0x8C };
static const u8 pat_rxctrl_b_new[] = { 0xE3,0xA0,0x10,0x18, 0xEA,0xFF,0xFF,0x8C };
// rxctrl C - MOV R1, #0x318 -> #0x118 (clear bits [10:9] only, preserve bit 8)
static const u8 pat_rxctrl_c[] = { 0xE3,0xA0,0x1F,0xC6 };
static const u8 pat_rxctrl_c_old[] = { 0xE3,0xA0,0x10,0x18 };
static const u8 pat_rxctrl_c_new[] = { 0xE3,0xA0,0x1F,0x46 };

// swrst init - set IPOSC bit, 0x44 -> 0xC4
static const u8 pat_swrst_init[] = { 0xE5,0x9A,0x00,0x00, 0xE3,0xA0,0x10,0x44 };
static const u8 pat_swrst_init_new[] = { 0xE5,0x9A,0x00,0x00, 0xE3,0xA0,0x10,0xC4 };
// swrst down - set IPOSC bit, 0x4C -> 0xCC
static const u8 pat_swrst_down[] = { 0xE5,0x94,0x00,0x00, 0xE3,0xA0,0x10,0x4C };
static const u8 pat_swrst_down_new[] = { 0xE5,0x94,0x00,0x00, 0xE3,0xA0,0x10,0xCC };

// VID:PID scanner prologue
static const u8 pat_vidpid_func[] = {
    0xE3,0xA0,0x20,0x00, 0xE1,0x52,0x00,0x01,
    0xE5,0x2D,0xE0,0x04, 0xE3,0xE0,0xE0,0x05,
    0x2A,0x00,0x00,0x09
};
// masks low nibble of PID so 0x7720..0x772F all match
static const u8 pat_vidpid_func_new[] = {
    0xE3,0xA0,0x20,0x00, 0xE1,0x52,0x00,0x01,
    0xE5,0x2D,0xE0,0x04, 0xE3,0xE0,0xE0,0x05,
    0x2A,0x00,0x00,0x08, 0xE5,0x9F,0xC0,0x30,
    0xE5,0x90,0x30,0x04, 0xE3,0xC3,0x30,0x0F,
    0xE1,0x53,0x00,0x0C, 0xE2,0x82,0x20,0x01,
    0xE2,0x80,0x00,0x08, 0x0A,0x00,0x00,0x04,
    0xE1,0x52,0x00,0x01, 0x3A,0xFF,0xFF,0xF7,
    0xE1,0xA0,0x00,0x0E, 0xE4,0x9D,0xE0,0x04,
    0xE1,0x2F,0xFF,0x1E, 0xE3,0xA0,0xE0,0x00,
    0xEA,0xFF,0xFF,0xFA, 0x0B,0x95,0x77,0x20
};

// thumb patterns (IOS57/58/59)
static const u8 pat_ios58_vidpid[] = { 0x0B,0x95,0x77,0x20, 0x00,0xFF,0xFF,0xFF };
// rxctrl MOV variants + following insn
static const u8 pat_ios58_rxc_318[] = { 0x21,0xC6, 0x00,0x89 };
static const u8 pat_ios58_rxc_218[] = { 0x21,0x86, 0x00,0x89 };
static const u8 pat_ios58_rxc_118[] = { 0x21,0x46, 0x00,0x89 };
static const u8 pat_ios58_rxc_018[] = { 0x21,0x06, 0x00,0x89 };
// swrst 0x4C -> 0xCC
static const u8 pat_ios58_swrst[] = { 0x21,0x4C };
static const u8 pat_ios58_swrst_new[] = { 0x21,0xCC };

s32 patch_arm32_ethernet(IOS *ios)
{
    tmd *t = (tmd *)SIGNATURE_PAYLOAD(ios->tmd);
    tmd_content *cr = TMD_CONTENTS(t);
    u8 hash[20];
    int applied = 0, existing = 0;
    s32 r;

    s32 index = find_eth_module(ios);
    if (index < 0) return -1;
    printf("eth module is content #%d\n", index);

    u8 *buf = ios->decrypted_buffer[index];
    u32 size = (u32)cr[index].size;

    r = patch_byte(buf, size, pat_ehc, sizeof(pat_ehc), 21, 0x30, 0x62, "ehc path");
    if (r < 0) return -1;
    if (r == 0) applied++; else existing++;

    r = patch_byte(buf, size, pat_oh0, sizeof(pat_oh0), 21, 0x30, 0x62, "oh0 path");
    if (r < 0) return -1;
    if (r == 0) applied++; else existing++;

    {
        s32 off = find_pattern(buf, size, pat_mov_pid, sizeof(pat_mov_pid));
        if (off < 0 || (u32)(off + 20) > size) { printf("  pid imm: not found\n"); return -1; }
        // ADD R3 is 16 bytes after the MOV R3
        u8 *add = buf + off + 16;
        if (add[0] != 0xE2 || add[1] != 0x83 || add[2] != 0x30) {
            printf("  pid imm: unexpected ADD\n"); return -1;
        }
        if (add[3] == 0x2B) { printf("  pid imm: already 0x2B\n"); existing++; }
        else if (add[3] == 0x20) {
            printf("  pid imm @ 0x%04X: 0x20 -> 0x2B\n", (s32)(add - buf) + 3);
            add[3] = 0x2B; applied++;
        }
        else { printf("  pid imm: unexpected 0x%02X\n", add[3]); return -1; }
    }

    r = patch_block_compat(buf, size, pat_rxctrl_b, pat_rxctrl_b_old, pat_rxctrl_b_new, sizeof(pat_rxctrl_b), "rxctrl B");
    if (r < 0) return -2;
    if (r == 0) applied++; else existing++;

    r = patch_block_compat(buf, size, pat_rxctrl_c, pat_rxctrl_c_old, pat_rxctrl_c_new, sizeof(pat_rxctrl_c), "rxctrl C");
    if (r < 0) return -2;
    if (r == 0) applied++; else existing++;

    r = patch_block(buf, size, pat_swrst_init, pat_swrst_init_new, sizeof(pat_swrst_init), "swrst init");
    if (r < 0) return -1;
    if (r == 0) applied++; else existing++;

    r = patch_block(buf, size, pat_swrst_down, pat_swrst_down_new, sizeof(pat_swrst_down), "swrst down");
    if (r < 0) return -1;
    if (r == 0) applied++; else existing++;

    {
        s32 off = find_pattern(buf, size, pat_vidpid_func, sizeof(pat_vidpid_func));
        if (off >= 0 && (u32)(off + 80) <= size) {
            printf("  vidpid scanner @ 0x%04X\n", off);
            memcpy(buf + off, pat_vidpid_func_new, 80);
            applied++;
        } else if (find_pattern(buf, size, pat_vidpid_func_new, 20) >= 0) {
            printf("  vidpid scanner: already done\n");
            existing++;
        } else {
            printf("  vidpid scanner: not found\n");
            return -1;
        }
    }

    if (applied == 0) { printf("ARM32: all patches already applied\n"); return 1; }

    t->contents[index].type = 1;
    SHA1(buf, size, hash);
    memcpy(cr[index].hash, hash, 20);
    printf("ARM32: %d new + %d existing on content #%d\n", applied, existing, index);
    return 0;
}

s32 patch_thumb_ethernet(IOS *ios)
{
    tmd *t = (tmd *)SIGNATURE_PAYLOAD(ios->tmd);
    tmd_content *cr = TMD_CONTENTS(t);
    u8 hash[20];
    int vidpid_hits = 0;
    s32 r;

    printf("scanning contents for VID:PID...\n");
    for (int i = 0; i < ios->content_count; i++) {
        if (!ios->decrypted_buffer[i]) continue;
        u8 *buf = ios->decrypted_buffer[i];
        u32 sz = (u32)cr[i].size;

        s32 off = find_pattern(buf, sz, pat_ios58_vidpid, sizeof(pat_ios58_vidpid));
        if (off >= 0) {
            printf("  content #%d: VID:PID @ 0x%04X -> 0x772B\n", i, off + 2);
            buf[off + 3] = 0x2B;
            t->contents[i].type = 1;
            SHA1(buf, sz, hash);
            memcpy(cr[i].hash, hash, 20);
            vidpid_hits++;
            continue;
        }
        u8 done[] = { 0x0B,0x95,0x77,0x2B, 0x00,0xFF,0xFF,0xFF };
        if (find_pattern(buf, sz, done, sizeof(done)) >= 0) {
            printf("  content #%d: VID:PID already 0x772B\n", i);
            vidpid_hits++;
        }
    }

    if (vidpid_hits == 0) { printf("no VID:PID found\n"); return -1; }
    printf("VID:PID patched/verified in %d content(s)\n", vidpid_hits);

    s32 eth = find_content_with(ios, pat_ios58_rxc_318, sizeof(pat_ios58_rxc_318));
    if (eth < 0)
        eth = find_content_with(ios, pat_ios58_rxc_118, sizeof(pat_ios58_rxc_118));
    if (eth < 0) {
        if (find_content_with(ios, pat_ios58_rxc_018, sizeof(pat_ios58_rxc_018)) >= 0) {
            printf("register patches already applied\n");
            return (vidpid_hits > 0) ? 0 : 1;
        }
        return -1;
    }

    printf("ETH driver is content #%d\n", eth);
    u8 *buf = ios->decrypted_buffer[eth];
    u32 size = (u32)cr[eth].size;
    int applied = 0, existing = 0;

    r = patch_block(buf, size, pat_ios58_rxc_318, pat_ios58_rxc_118, sizeof(pat_ios58_rxc_318), "rxctrl EHC");
    if (r < 0) r = patch_block(buf, size, pat_ios58_rxc_018, pat_ios58_rxc_118, sizeof(pat_ios58_rxc_018), "rxctrl EHC (fix 0x018)");
    if (r < 0) return -1;
    if (r == 0) applied++; else existing++;

    r = patch_block(buf, size, pat_ios58_rxc_218, pat_ios58_rxc_018, sizeof(pat_ios58_rxc_218), "rxctrl OH0");
    if (r < 0) r = patch_block(buf, size, pat_ios58_rxc_118, pat_ios58_rxc_018, sizeof(pat_ios58_rxc_118), "rxctrl OH0 (fix 0x118)");
    if (r < 0) return -1;
    if (r == 0) applied++; else existing++;

    {
        int n = count_pattern(buf, size, pat_ios58_swrst, sizeof(pat_ios58_swrst));
        int n_done = count_pattern(buf, size, pat_ios58_swrst_new, sizeof(pat_ios58_swrst_new));
        if (n == 1 && n_done == 0) {
            r = patch_block(buf, size, pat_ios58_swrst, pat_ios58_swrst_new, sizeof(pat_ios58_swrst), "swrst down");
            if (r < 0) return -1;
            if (r == 0) applied++; else existing++;
        } else if (n == 0 && n_done == 1) {
            printf("  swrst down: already done\n"); existing++;
        } else if (n > 1) {
            printf("  swrst down: %d matches, skipping\n", n);
        } else {
            printf("  swrst down: not found\n"); return -1;
        }
    }

    if (applied > 0) {
        t->contents[eth].type = 1;
        SHA1(buf, size, hash);
        memcpy(cr[eth].hash, hash, 20);
    }

    printf("Thumb: %d new + %d existing on content #%d\n", applied, existing, eth);
    return (applied > 0 || vidpid_hits > 0) ? 0 : 1;
}
