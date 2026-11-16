// cIOS: the custom IOS of the USB loaders
//
// A USB loader (e.g. USB Loader GX) starts a game from a USB drive. The stock IOS cannot read from
// a USB drive, so the loader runs the game on a "cIOS" instead. The d2x cIOS installer builds one
// from a stock IOS (the "base"), replaces some of its modules and installs it into a free slot,
// usually 248 to 251, with version 65535.
//
// The ETH module (the network driver) of a d2x cIOS is the one of its base, byte for byte. Tested
// with d2x v11 beta1 on a Wii:
//   IOS248 (base 38) and IOS249 (base 56)   IOS36-type ETH module
//   IOS250 (base 57)                        IOS57-type ETH module
//   IOS251 (base 58)                        IOS58-type ETH module
// The same with d2x v10 (version 21010) on a vWii, slots 249 to 251 (248 is not used there):
//   IOS249   IOS36-type ETH module
//   IOS250   IOS57-type ETH module
//   IOS251   IOS58-type ETH module
// So the AX88179 patch of patch_179.c fits a cIOS without any new patch data.
//
// The difference that matters: a cIOS is not signed by Nintendo. Its TMD is fakesigned (signature
// all zeros), just like an IOS this tool installed. So the signature alone cannot tell
// "a cIOS" from "patched by this tool". The ETH module can:
//   fakesigned + the ETH module is a stock one       ->  cIOS     (nobody patched it)
//   fakesigned + an ETH module that is not (the AX88179 driver, or the AX88772B / C / D patch,
//                which only change bytes inside the module)  ->  MODIFIED (patched by this tool)
// Only the TMD is needed for that: it has the size and the hash of every content. A content with
// the size of an ETH module but another hash is a changed ETH module.
//
// A cIOS ticket has key index 0 (Wii common key) on the Wii and on the vWii, so nothing about the
// vWii key index 2 of the stock vWii IOS applies here (install_IOS() puts the ticket back anyway).

#include <string.h>
#include <gccore.h>
#include "cios.h"
#include "sigcheck.h"
#include "patch_179.h"
#include "IOSPatcher.h"

// the slots the d2x installer uses by default
static const u32 cios_slots[] = { 248, 249, 250, 251 };
#define CIOS_SLOT_COUNT (sizeof(cios_slots) / sizeof(cios_slots[0]))

bool cios_is_slot(u32 iosnr)
{
    for (u32 k = 0; k < CIOS_SLOT_COUNT; k++) {
        if (cios_slots[k] == iosnr)
            return true;
    }
    return false;
}

// A cIOS slot is only shown if something is installed there. A console without cIOS shows the
// same list as before.
void cios_add_installed_slots(u32 *list, u32 *count, u32 max)
{
    for (u32 k = 0; k < CIOS_SLOT_COUNT; k++) {
        u32 iosnr = cios_slots[k];
        if (checkIOS(iosnr) < 0)
            continue;                 // nothing installed in this slot
        if (*count < max) {
            list[*count] = iosnr;
            (*count)++;
        }
    }
}

// size of a signed TMD without contents, and of one content entry (as in backup.c)
#define TMD_HEADER_SIZE  0x1e4
#define TMD_CONTENT_SIZE 36

// Looks for a changed ETH module in the content list of the TMD: a content with the size of a stock
// ETH module whose hash is not a stock one. Returns 1 if there is one, 0 if not, -1 if the TMD (len
// bytes) is too short for its content list.
static int tmd_has_changed_eth(const void *signed_tmd, u32 len)
{
    const tmd *t = (const tmd *)SIGNATURE_PAYLOAD((const signed_blob *)signed_tmd);

    if (len < TMD_HEADER_SIZE + TMD_CONTENT_SIZE * (u32)t->num_contents)
        return -1;

    for (u32 i = 0; i < t->num_contents; i++) {
        if (ax179_is_eth_size((u32)t->contents[i].size) && !ax179_is_stock_eth_hash(t->contents[i].hash))
            return 1;
    }
    return 0;
}

int ios_sig_class(u32 iosnr, const void *signed_tmd, u32 len)
{
    int sig = tmd_sig_class(signed_tmd, len);

    // only a fakesigned IOS in a cIOS slot can be a cIOS
    if (sig != SIG_FAKE || !cios_is_slot(iosnr))
        return sig;

    // A changed ETH module in it, or a TMD that cannot be read to the end: MODIFIED.
    // (MODIFIED is the safe answer: such an IOS is never dumped and never used as a base.)
    if (tmd_has_changed_eth(signed_tmd, len) != 0)
        return SIG_FAKE;
    return SIG_CIOS;
}
