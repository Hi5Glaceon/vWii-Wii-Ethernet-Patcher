#ifndef _BACKUP_H_
#define _BACKUP_H_

#include <gccore.h>

// where an IOS backup lives
#define BK_NONE    0
#define BK_STOCK   1   // <plat dir>/IOSn-64-vV.wad          Nintendo-signed
#define BK_CUSTOM  2   // <plat dir>/IOSn-64-vV-custom.wad   changed by another tool (e.g. WiiLink)
#define BK_LEGACY  3   // sd:/wad/IOSn-64-vV.wad             written by the original tool and older versions (read only)

typedef struct {
    int  kind;          // BK_*
    int  sig;           // SIG_* of the backup's TMD
    char path[80];
} backup_match;

void backup_path(char *out, size_t size, int kind, u32 iosnr, u32 ver);

// short name for messages: "stock", "custom", "old sd:/wad/"
const char *backup_kind_name(int kind);

// reads only the TMD (content list) of a WAD; IOS number and version must match. 0 ok, <0 error
s32 wad_read_tmd(const char *path, u32 iosnr, u32 ver, signed_blob **tmd, u32 *len);

// Finds the backup the IOS on the NAND was made from: same IOS, same version, and all contents
// the same as on the NAND - except the ETH module, if the NAND has a patched one.
// Stock is preferred, then custom, then old sd:/wad/ backups. 0 found, -1 none
s32 backup_find(u32 iosnr, u32 ver, const signed_blob *nand_tmd, backup_match *out);

// Finds a backup signed by Nintendo with this version, no matter what is on the NAND (to restore
// an IOS that another tool changed completely). 0 found, -1 none
s32 backup_find_nintendo(u32 iosnr, u32 ver, backup_match *out);

#endif
