// Is this a Wii or a Wii U (vWii)?
//
// The Wii U has a newer chip ("Latte"). Its chip revision register has 0xCAFE in the upper 16 bits.
// Other homebrew checks it the same way. Reading the register needs AHBPROT, which the patcher
// has anyway.

#include <gccore.h>
#include "platform.h"
#include "patch_179.h"
#include "IOSPatcher.h"
#include "cios.h"

// the chip revision register
#define LT_CHIPREVID (*(vu32 *)0xCD8005A0)

static bool checked = false;     // was the register read already?
static bool is_vwii = false;     // the result

bool plat_is_vwii(void)
{
    if (!checked) {
        u32 upper = LT_CHIPREVID >> 16;
        is_vwii = (upper == 0xCAFE);
        checked = true;
    }
    return is_vwii;
}

const char *plat_name(void)
{
    if (plat_is_vwii())
        return "vWii";
    return "Wii";
}

const char *plat_wad_dir(void)
{
    if (plat_is_vwii())
        return "sd:/wad/vwii";
    return "sd:/wad/wii";
}

u32 plat_ios[PLAT_MAX_IOS];
u32 plat_ios_count = 0;

// Makes the IOS list of this console from the list in patch_179.c.
// IOS59 only exists on the Wii U. A Wii only lists it if it is installed after all.
// At the end come the cIOS slots 248 to 251 that are installed (see cios.c).
void plat_init(void)
{
    plat_ios_count = 0;

    for (u32 k = 0; k < ax179_supported_count; k++) {
        u32 iosnr = ax179_supported_ios[k];

        if (iosnr == 59 && !plat_is_vwii() && checkIOS(iosnr) < 0)
            continue;            // Wii without IOS59

        if (plat_ios_count < PLAT_MAX_IOS) {
            plat_ios[plat_ios_count] = iosnr;
            plat_ios_count++;
        }
    }

    cios_add_installed_slots(plat_ios, &plat_ios_count, PLAT_MAX_IOS);
}
