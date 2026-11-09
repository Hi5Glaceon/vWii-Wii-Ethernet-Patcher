#ifndef _PLATFORM_H_
#define _PLATFORM_H_

#include <gccore.h>

// Is this a Wii or a Wii U (vWii)? See platform.c.

// true on a Wii U (vWii), false on a Wii
bool plat_is_vwii(void);

// "vWii" or "Wii"
const char *plat_name(void);

// the backup folder of this console: "sd:/wad/vwii" or "sd:/wad/wii"
const char *plat_wad_dir(void);

// The IOS this console works with (dump, patch, show on the screen).
// plat_init() fills the list.
extern u32 plat_ios[];
extern u32 plat_ios_count;
void plat_init(void);

#endif
