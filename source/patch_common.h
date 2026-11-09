#ifndef _PATCH_COMMON_H_
#define _PATCH_COMMON_H_

#include <gccore.h>
#include <stdio.h>
#include <string.h>
#include "IOSPatcher.h"
#include "sha1.h"

// find pat in buf, returns offset or -1
static inline s32 find_pattern(const u8 *buf, u32 size, const u8 *pat, u32 len)
{
    if (size < len) return -1;
    for (u32 i = 0; i <= size - len; i++)
        if (memcmp(buf + i, pat, len) == 0)
            return (s32)i;
    return -1;
}

// find which IOS content contains pat
static inline s32 find_content_with(IOS *ios, const u8 *pat, u32 len)
{
    tmd *t = (tmd *)SIGNATURE_PAYLOAD(ios->tmd);
    tmd_content *cr = TMD_CONTENTS(t);
    for (int i = 0; i < ios->content_count; i++) {
        if (!ios->decrypted_buffer[i]) continue;
        if (find_pattern(ios->decrypted_buffer[i], (u32)cr[i].size, pat, len) >= 0)
            return i;
    }
    return -1;
}

// EHCI device path "/dev/usb/ehc/0b95"
static const u8 _pat_ehc_base[] = {
    0x2F,0x64,0x65,0x76,0x2F,0x75,0x73,0x62,
    0x2F,0x65,0x68,0x63,0x2F,0x30,0x62,0x39,0x35
};
// OHCI device path "/dev/usb/oh0/0b95"
static const u8 _pat_oh0_base[] = {
    0x2F,0x64,0x65,0x76,0x2F,0x75,0x73,0x62,
    0x2F,0x6F,0x68,0x30,0x2F,0x30,0x62,0x39,0x35
};

static inline s32 find_eth_module(IOS *ios)
{
    s32 idx = find_content_with(ios, _pat_ehc_base, sizeof(_pat_ehc_base));
    return (idx >= 0) ? idx : find_content_with(ios, _pat_oh0_base, sizeof(_pat_oh0_base));
}

// patch one byte at offset from pattern match
static inline s32 patch_byte(u8 *buf, u32 size, const u8 *base, u32 baselen,
    u32 offset, u8 expect, u8 value, const char *name)
{
    s32 off = find_pattern(buf, size, base, baselen);
    if (off < 0 || (u32)(off + offset + 1) > size) {
        printf("  %s: not found\n", name);
        return -1;
    }
    u8 c = buf[off + offset];
    if (c == value) { printf("  %s: already 0x%02X\n", name, value); return 1; }
    if (c != expect) { printf("  %s: unexpected 0x%02X\n", name, c); return -1; }
    printf("  %s @ 0x%04X: 0x%02X -> 0x%02X\n", name, off + offset, c, value);
    buf[off + offset] = value;
    return 0;
}

// find and replace a byte sequence
static inline s32 patch_block(u8 *buf, u32 size, const u8 *old, const u8 *repl,
    u32 len, const char *name)
{
    s32 off = find_pattern(buf, size, old, len);
    if (off >= 0) {
        printf("  %s @ 0x%04X\n", name, off);
        memcpy(buf + off, repl, len);
        return 0;
    }
    if (find_pattern(buf, size, repl, len) >= 0) {
        printf("  %s: already done\n", name);
        return 1;
    }
    printf("  %s: not found\n", name);
    return -1;
}

// try stock value first, then old patcher value, replace with new
static inline s32 patch_block_compat(u8 *buf, u32 size,
    const u8 *stock, const u8 *old_patch, const u8 *new_patch,
    u32 len, const char *name)
{
    s32 r = patch_block(buf, size, stock, new_patch, len, name);
    if (r >= 0) return r;
    r = patch_block(buf, size, old_patch, new_patch, len, name);
    if (r >= 0) return r;
    return -1;
}

static inline int count_pattern(const u8 *buf, u32 size, const u8 *pat, u32 len)
{
    int n = 0;
    if (size < len) return 0;
    for (u32 i = 0; i <= size - len; i++)
        if (memcmp(buf + i, pat, len) == 0) n++;
    return n;
}

#endif
