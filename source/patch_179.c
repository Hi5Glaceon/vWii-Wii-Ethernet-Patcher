// Puts the AX88179 / AX88179A driver into the ETH module of an IOS.
//
// The ETH module is the network driver inside an IOS. Nintendo's driver only knows the AX88772.
// This file overwrites parts of it with a new driver for the AX88179 family.
//
// How it works:
//   1. Look at every content (file) of the IOS. If its SHA-1 hash is the hash of one of the four
//      known stock ETH modules, it is the one to patch.
//   2. Copy the patch records into it. A patch record is "at offset X write these Y bytes".
//   3. Check the SHA-1 of the result. It must be exactly the expected hash, otherwise nothing is
//      used. So only these exact modules are ever patched, and always to the same result.
//   4. Write the new hash into the TMD (the content list of the IOS).
//
// The four known ETH modules:
//   IOS58-type  Thumb code, in IOS58 v6432 (also the Wii IOS58 v6176)
//   IOS57-type  Thumb code, in IOS57 v6175 (also the Wii IOS57 v5919)
//   IOS59-type  Thumb code, in IOS59 v9249
//   IOS36-type  ARM code, in IOS36 v3864 and in IOS31, 33, 34, 35, 37, 38, 41, 43, 45, 46, 48,
//               53, 55, 56, 62 and 80 (they all carry the very same module)
//
// The patch records and the hashes are in the ax179_ios*_data.h files. They are made by the
// scripts in tools/ax179/ from the stock modules.

#include <stdio.h>
#include <string.h>
#include <gccore.h>
#include "patch_common.h"
#include "patch_179.h"
#include "sha1.h"
#include "tools.h"
#include "ax179_ios58_data.h"
#include "ax179_ios57_data.h"
#include "ax179_ios59_data.h"
#include "ax179_ios36_data.h"

// The data files say if the driver links at 1000M (the GIGABIT line). Data files made before that line
// existed contain a driver with gigabit, which was not reliable, so they are not accepted.
#if !defined(AX179_IOS36_GIGABIT) || !defined(AX179_IOS58_GIGABIT) || !defined(AX179_IOS57_GIGABIT) || !defined(AX179_IOS59_GIGABIT)
#error "The AX88179 patch data is out of date: run build_patch36.py and build_patch.py 58 / 57 / 59 again (see tools/ax179/README.md)"
#endif

// everything we know about one stock ETH module
typedef struct {
    const char *name;                     // for the screen, e.g. "IOS58-type"
    u32 size;                             // size of the module in bytes
    const u8 *orig_sha1;                  // hash of the stock module
    const u8 *new_sha1;                   // hash after patching
    const ax179_patch_rec_t *patches;     // the patch records
    u32 count;                            // number of patch records
    u32 gigabit;                          // 1 if the driver links at 1000M, 0 for 10/100M
} ax179_module_t;

#define MODULE_COUNT 4

static const ax179_module_t modules[MODULE_COUNT] = {
    { "IOS58-type", AX179_IOS58_MODULE_SIZE, ax179_ios58_orig_sha1, ax179_ios58_new_sha1,
      ax179_ios58_patches, AX179_IOS58_PATCH_COUNT, AX179_IOS58_GIGABIT },
    { "IOS57-type", AX179_IOS57_MODULE_SIZE, ax179_ios57_orig_sha1, ax179_ios57_new_sha1,
      ax179_ios57_patches, AX179_IOS57_PATCH_COUNT, AX179_IOS57_GIGABIT },
    { "IOS59-type", AX179_IOS59_MODULE_SIZE, ax179_ios59_orig_sha1, ax179_ios59_new_sha1,
      ax179_ios59_patches, AX179_IOS59_PATCH_COUNT, AX179_IOS59_GIGABIT },
    { "IOS36-type", AX179_IOS36_MODULE_SIZE, ax179_ios36_orig_sha1, ax179_ios36_new_sha1,
      ax179_ios36_patches, AX179_IOS36_PATCH_COUNT, AX179_IOS36_GIGABIT },
};

// every IOS that has one of the four modules, in the order they are patched
const u32 ax179_supported_ios[] = {
    31, 33, 34, 35, 36, 37, 38,
    41, 43, 45, 46, 48,
    53, 55, 56, 57, 58, 59,
    62, 80
};
// the number of entries in the list above (size of the list / size of one entry)
const u32 ax179_supported_count = sizeof(ax179_supported_ios) / sizeof(ax179_supported_ios[0]);

// true if the two 20-byte hashes are the same
static bool same_hash(const u8 *a, const u8 *b)
{
    return memcmp(a, b, 20) == 0;
}

bool ax179_is_stock_eth_hash(const u8 *sha1)
{
    for (int m = 0; m < MODULE_COUNT; m++) {
        if (same_hash(sha1, modules[m].orig_sha1))
            return true;
    }
    return false;
}

// Patches content number i of the IOS (buf, a stock module of the kind md) and writes the new
// hash into the TMD. Returns 0 if it worked, a negative number if the patch data is broken
// (then the caller throws the buffer away).
static s32 patch_module(tmd *t, u32 i, u8 *buf, const ax179_module_t *md)
{
    printf("  content #%u: %s ETH module found\n", i, md->name);

    // check first that no record writes past the end of the module
    for (u32 p = 0; p < md->count; p++) {
        const ax179_patch_rec_t *rec = &md->patches[p];
        if (rec->offset + rec->len > md->size) {
            printf("  patch record %u out of range, aborting\n", p);
            return -1;
        }
    }

    // copy every record into the module
    for (u32 p = 0; p < md->count; p++) {
        const ax179_patch_rec_t *rec = &md->patches[p];
        memcpy(buf + rec->offset, rec->repl, rec->len);
    }

    // the result must be exactly the expected module
    u8 hash[20];
    SHA1(buf, md->size, hash);
    if (!same_hash(hash, md->new_sha1)) {
        printf("  result hash mismatch, aborting\n");
        write_log("patch: %s result hash mismatch", md->name);
        return -2;
    }

    printf("  AX88179 driver + call sites + device match patched (%u records)\n", (unsigned)md->count);
    if (md->gigabit)
        write_log("patch: %s driver, AX88179 link up to 1000M", md->name);
    else
        write_log("patch: %s driver, AX88179 link 10/100M only", md->name);

    // enter the module in the TMD: new hash, and type 1 ("normal") because the patched module
    // is no longer the same as the shared one other IOS use
    t->contents[i].type = 1;
    memcpy(t->contents[i].hash, hash, 20);
    return 0;
}

// Looks at a content of the IOS (size bytes) and tells which ETH module it is.
// Returns NULL if it is none of the known ones. *is_patched is true if it is the patched version.
static const ax179_module_t *identify_module(u8 *buf, u32 size, bool *is_patched)
{
    for (int m = 0; m < MODULE_COUNT; m++) {
        const ax179_module_t *md = &modules[m];

        // a content of the wrong size cannot be this module (and hashing it is not needed)
        if (size != md->size)
            continue;

        u8 hash[20];
        SHA1(buf, md->size, hash);

        if (same_hash(hash, md->orig_sha1)) {
            *is_patched = false;
            return md;
        }
        if (same_hash(hash, md->new_sha1)) {
            *is_patched = true;
            return md;
        }
    }
    return NULL;
}

s32 patch_179(IOS *ios)
{
    tmd *t = (tmd *)SIGNATURE_PAYLOAD(ios->tmd);

    for (u32 i = 0; i < ios->content_count; i++) {
        u8 *buf = ios->decrypted_buffer[i];
        if (buf == NULL)
            continue;

        bool is_patched = false;
        const ax179_module_t *md = identify_module(buf, (u32)t->contents[i].size, &is_patched);
        if (md == NULL)
            continue;                     // this content is not an ETH module

        if (is_patched) {
            printf("  content #%u: AX88179 driver already installed\n", i);
            return 1;
        }
        return patch_module(t, i, buf, md);
    }

    write_log("patch: no supported stock ETH module");
    return 2;
}
