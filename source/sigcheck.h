#ifndef _SIGCHECK_H_
#define _SIGCHECK_H_

#include <gccore.h>

// the three kinds of IOS (see sigcheck.c)
#define SIG_NINTENDO 0   // "stock":    signed by Nintendo
#define SIG_FAKE     1   // "MODIFIED": the signature is all zeros (installed by a patcher)
#define SIG_OTHER    2   // "custom":   another signature (changed by another tool, e.g. WiiLink)

// Looks at the signature of a TMD (len bytes, signature included).
// Returns SIG_NINTENDO, SIG_FAKE or SIG_OTHER.
int tmd_sig_class(const void *signed_tmd, u32 len);

// "stock", "MODIFIED" or "custom"
const char *sig_class_name(int c);

#endif
