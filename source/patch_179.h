#ifndef _PATCH_179_H_
#define _PATCH_179_H_

#include "IOSPatcher.h"

// One patch record: write len bytes from repl into the ETH module at offset.
// (The ax179_ios*_data.h files are lists of these.)
typedef struct {
    u32 offset;
    u32 len;
    const u8 *repl;
} ax179_patch_rec_t;

// the IOS that have an ETH module this tool can patch, and how many there are
extern const u32 ax179_supported_ios[];
extern const u32 ax179_supported_count;

// The System Menu runs on IOS80. It is only installed after its own question.
#define AX179_SYSMENU_IOS 80

// Puts the AX88179 driver into the ETH module of the IOS (in memory).
// Returns 0 patched, 1 already patched, 2 no ETH module this tool knows, <0 error.
s32 patch_179(IOS *ios);

// true if sha1 is the hash of one of the stock ETH modules this tool knows
bool ax179_is_stock_eth_hash(const u8 *sha1);

#endif
