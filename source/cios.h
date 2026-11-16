#ifndef _CIOS_H_
#define _CIOS_H_

#include <gccore.h>

// cIOS: the custom IOS that USB loaders use (see cios.c).

// true if iosnr is one of the cIOS slots this tool looks at (248 to 251)
bool cios_is_slot(u32 iosnr);

// Adds the installed cIOS slots to the IOS list of this console (see cios.c).
void cios_add_installed_slots(u32 *list, u32 *count, u32 max);

// Like tmd_sig_class() in sigcheck.c, but knows one more kind of IOS:
//   SIG_CIOS  a cIOS in slot 248 to 251 that this tool did not patch
// Everything else gets the same answer as from tmd_sig_class().
int ios_sig_class(u32 iosnr, const void *signed_tmd, u32 len);

#endif
