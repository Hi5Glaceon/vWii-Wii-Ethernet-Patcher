// IOS backups on the SD card
//
// Every console keeps its backups in its own folder: sd:/wad/wii or sd:/wad/vwii.
// There are three kinds of backup files:
//   stock   IOSn-64-vV.wad          the IOS as Nintendo made it
//   custom  IOSn-64-vV-custom.wad   an IOS another tool changed (e.g. the WiiLink IOS31 / IOS80)
//   legacy  sd:/wad/IOSn-64-vV.wad  written by the original tool and older versions, only read
//
// The file name alone never decides which backup is used. backup_find() compares the list of
// contents (the TMD) of a backup with the TMD of the IOS on the NAND.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gccore.h>
#include "backup.h"
#include "platform.h"
#include "sigcheck.h"
#include "patch_179.h"
#include "tools.h"
#include "memory/mem2.hpp"

// size of a TMD without contents, and of one content entry
#define TMD_HEADER_SIZE  0x1e4
#define TMD_CONTENT_SIZE 36

// the header at the start of every WAD file
typedef struct {
    u32 header_len;
    u16 type;
    u16 padding;
    u32 certs_len;
    u32 crl_len;
    u32 tik_len;
    u32 tmd_len;
    u32 data_len;
    u32 footer_len;
} ATTRIBUTE_PACKED bk_wad_header;

// every part of a WAD starts at a multiple of 64 bytes
static u32 round_up_64(u32 value)
{
    return (value + 63) / 64 * 64;
}

void backup_path(char *out, size_t size, int kind, u32 iosnr, u32 ver)
{
    if (kind == BK_STOCK)
        snprintf(out, size, "%s/IOS%u-64-v%u.wad", plat_wad_dir(), iosnr, ver);
    else if (kind == BK_CUSTOM)
        snprintf(out, size, "%s/IOS%u-64-v%u-custom.wad", plat_wad_dir(), iosnr, ver);
    else if (kind == BK_LEGACY)
        snprintf(out, size, "sd:/wad/IOS%u-64-v%u.wad", iosnr, ver);
    else
        out[0] = 0;
}

const char *backup_kind_name(int kind)
{
    if (kind == BK_STOCK)
        return "stock";
    if (kind == BK_CUSTOM)
        return "custom";
    if (kind == BK_LEGACY)
        return "old sd:/wad/";
    return "none";
}

// true if the header looks like the header of an IOS WAD
static bool header_ok(const bk_wad_header *h)
{
    if (h->header_len != 0x20)
        return false;
    if (h->certs_len == 0 || h->tik_len == 0)
        return false;
    if (h->tmd_len < TMD_HEADER_SIZE)
        return false;
    if (h->tmd_len > TMD_HEADER_SIZE + TMD_CONTENT_SIZE * 512)
        return false;
    return true;
}

// true if the TMD belongs to IOS iosnr in version ver and fits into len bytes
static bool tmd_ok(signed_blob *s, u32 len, u32 iosnr, u32 ver)
{
    if (!IS_VALID_SIGNATURE(s))
        return false;

    tmd *t = (tmd *)SIGNATURE_PAYLOAD(s);
    u64 title_id = ((u64)1 << 32) | iosnr;
    if (t->title_id != title_id)
        return false;
    if (t->title_version != ver)
        return false;
    if (TMD_HEADER_SIZE + TMD_CONTENT_SIZE * (u32)t->num_contents > len)
        return false;
    return true;
}

// Reads only the TMD out of a WAD file (the rest is not needed to compare backups).
s32 wad_read_tmd(const char *path, u32 iosnr, u32 ver, signed_blob **tmd_out, u32 *len_out)
{
    *tmd_out = NULL;
    *len_out = 0;

    FILE *fp = fopen(path, "rb");
    if (fp == NULL)
        return -1;

    // the header
    bk_wad_header h;
    if (fread(&h, sizeof(h), 1, fp) != 1 || !header_ok(&h)) {
        fclose(fp);
        return -2;
    }

    // the TMD comes after the header, the certificates, the CRL and the ticket
    u32 offset = round_up_64(h.header_len);
    offset = round_up_64(offset + h.certs_len);
    offset = round_up_64(offset + h.crl_len);
    offset = round_up_64(offset + h.tik_len);

    u8 *buf = MEM2_memalign(32, round_up_64(h.tmd_len));
    if (buf == NULL) {
        fclose(fp);
        return -3;
    }

    if (fseek(fp, offset, SEEK_SET) != 0 || fread(buf, h.tmd_len, 1, fp) != 1) {
        free(buf);
        fclose(fp);
        return -2;
    }
    fclose(fp);

    if (!tmd_ok((signed_blob *)buf, h.tmd_len, iosnr, ver)) {
        free(buf);
        return -4;
    }

    *tmd_out = (signed_blob *)buf;
    *len_out = h.tmd_len;
    return 0;
}

// Compares the content list of the IOS on the NAND with the one of a backup.
// Returns how many contents are different, or -1 if the two lists do not even have the same
// layout. *eth_only is set to false if a different content is not a known stock ETH module.
static int tmd_diff(const signed_blob *nand, const signed_blob *bk, bool *eth_only)
{
    const tmd *a = (const tmd *)SIGNATURE_PAYLOAD(nand);
    const tmd *b = (const tmd *)SIGNATURE_PAYLOAD(bk);

    *eth_only = true;
    if (a->title_id != b->title_id)
        return -1;
    if (a->title_version != b->title_version)
        return -1;
    if (a->num_contents != b->num_contents)
        return -1;

    int diff = 0;
    for (u32 i = 0; i < a->num_contents; i++) {
        const tmd_content *x = &a->contents[i];
        const tmd_content *y = &b->contents[i];

        if (x->cid != y->cid || x->index != y->index || x->size != y->size)
            return -1;

        if (memcmp(x->hash, y->hash, 20) == 0)
            continue;                 // same content

        diff++;
        if (!ax179_is_stock_eth_hash(y->hash))
            *eth_only = false;
    }
    return diff;
}

// True if a backup file can be the base for the IOS on the NAND.
//   kind, sig   what kind of backup it is and who signed it
//   diff        how many contents are different from the NAND (-1 = the lists do not fit together)
//   eth_only    true if every different content is a stock ETH module
static bool backup_fits(int kind, int sig, int diff, bool eth_only)
{
    if (sig == SIG_FAKE)
        return false;                 // a patched WAD is never used as a base
    if (kind == BK_STOCK && sig != SIG_NINTENDO)
        return false;                 // the stock file name promises a stock IOS
    if (diff < 0 || diff > 1)
        return false;                 // different layout, or more than one content differs
    if (diff == 1 && !eth_only)
        return false;                 // the one different content is not the ETH module
    return true;
}

// Finds the backup the IOS on the NAND was made from. A backup fits if
//   - it is not patched itself (not fakesigned),
//   - a file called "stock" really is signed by Nintendo,
//   - all its contents are the same as on the NAND, or only the ETH module is different
//     (then the NAND has the patched one).
// Stock is looked at first, then custom, then legacy. A backup with no difference at all wins
// over one where the ETH module is different.
s32 backup_find(u32 iosnr, u32 ver, const signed_blob *nand_tmd, backup_match *out)
{
    int kinds[3] = { BK_STOCK, BK_CUSTOM, BK_LEGACY };
    backup_match best;
    int best_diff = 99;               // number of differences of the best backup so far (99 = none yet)
    char note[96] = "";               // for the log: what was found in every backup file

    best.kind = BK_NONE;

    for (int k = 0; k < 3; k++) {
        int kind = kinds[k];
        char path[80];
        backup_path(path, sizeof(path), kind, iosnr, ver);

        signed_blob *t = NULL;
        u32 len = 0;
        if (wad_read_tmd(path, iosnr, ver, &t, &len) < 0)
            continue;                 // no such file (or not readable)

        int sig = tmd_sig_class(t, len);
        bool eth_only = false;
        int diff = tmd_diff(nand_tmd, t, &eth_only);
        free(t);

        // note for the log, e.g. " stock:stock/0" or " custom:custom/1eth"
        const char *eth = "";
        if (diff == 1 && eth_only)
            eth = "eth";
        size_t used = strlen(note);       // the new text is added behind the text that is already there
        snprintf(note + used, sizeof(note) - used, " %s:%s/%d%s",
                 backup_kind_name(kind), sig_class_name(sig), diff, eth);

        if (!backup_fits(kind, sig, diff, eth_only))
            continue;

        if (diff < best_diff) {
            best_diff = diff;
            best.kind = kind;
            best.sig = sig;
            strcpy(best.path, path);
        }
    }

    if (best.kind == BK_NONE) {
        if (note[0] == 0)
            write_log("IOS%u v%u: no matching backup ( no backup file)", iosnr, ver);
        else
            write_log("IOS%u v%u: no matching backup (%s)", iosnr, ver, note);
        return -1;
    }

    *out = best;
    return 0;
}

// Finds a backup signed by Nintendo with this version, no matter what is on the NAND.
// Used to restore an IOS that another tool changed completely.
s32 backup_find_nintendo(u32 iosnr, u32 ver, backup_match *out)
{
    int kinds[2] = { BK_STOCK, BK_LEGACY };

    for (int k = 0; k < 2; k++) {
        char path[80];
        backup_path(path, sizeof(path), kinds[k], iosnr, ver);

        signed_blob *t = NULL;
        u32 len = 0;
        if (wad_read_tmd(path, iosnr, ver, &t, &len) < 0)
            continue;

        int sig = tmd_sig_class(t, len);
        free(t);
        if (sig != SIG_NINTENDO)
            continue;

        out->kind = kinds[k];
        out->sig = sig;
        strcpy(out->path, path);
        return 0;
    }
    return -1;
}
