# vWii Ethernet Patcher

## AI has been used for this documentation and analysis.

Patches the Wii and vWii IOS so that newer USB ethernet adapters work: AX88772B, AX88772C, AX88772D,
AX88179 (Nintendo Switch LAN adapter) and AX88179A.

**Status: v2.0-beta.** This is based on doworian's vWii Ethernet Patcher. New compared with the original: the
Nintendo Switch LAN adapter (AX88179) and the AX88179A, support for the Wii (not only the vWii), a dry run before
installing, restore, and separate backups per console. The patcher writes to the NAND, so please read
"Requirements" first.

The stock IOS only know the original AX88772. Newer chips use other product IDs and other registers.
This tool changes the ETH module of the 20 IOS that contain one; everything else stays as it is.

## Requirements

- Wii or Wii U (vWii) with the Homebrew Channel
- SD card
- **A NAND backup before installing anything:** vWii `slccmpt.bin` + `otp.bin`, Wii BootMii `nand.bin` + `keys.bin`
- Recommended: vWii Priiloader + Aroma, Wii BootMii (as boot2 if possible) + Priiloader

## Usage

1. Copy `boot.dol` and `meta.xml` to `sd:/apps/vwii_ethernet_patcher/` and start it from the Homebrew Channel.
2. Plug in the adapter. It is shown at the bottom right of the main screen.
3. **[+] Dump** the IOS to the SD card first (`sd:/wad/vwii/` on a Wii U, `sd:/wad/wii/` on a Wii).
   The patcher builds the patched IOS from these backups, not from the NAND.
4. **[A] Patch** (only offered when a supported adapter is plugged in).
5. Power the console off and on again.
6. **[B] Test network** checks DHCP, DNS and the internet connection. Afterwards the patcher closes and
   returns to the Homebrew Channel (the IOS network keeps running and takes the adapter away, so the
   menu would no longer see it).

**[-] Restore** writes every patched IOS back to the state before patching. Use it to go back to the
AX88772 or after problems. **[HOME]** exits.

### AX88179 / AX88179A

[A] opens a list of the IOS of the console: D-Pad to move, [A] to select, [+] all, [-] none, [1] start.
First a **dry run** writes the patched WADs to `sd:/wad/<wii|vwii>/ax179/`. Installing is a separate step
that you confirm with [1] [2] [A]. IOS80 (System Menu) is asked for on its own and can be left out.

### Link speed: use 100M

**Gigabit caused problems in the tests.** On a Wii in WiiConnect24 standby the link came up at 1000M for a
moment, dropped and settled at 100M; until then the connection test needed several attempts and the Wii
Message Board failed with error 51420. At 100M everything worked. The Wii cannot use more than 100M anyway
(USB 2.0).

- **AX88179:** the driver only offers 10/100M, nothing to do.
- **AX88179A:** the adapter negotiates the speed itself and the driver cannot limit it. **Limit the switch
  or router port of the console to 100M.** The patcher reminds you when an AX88179A is plugged in.
- Use an auto-negotiation setting limited to 100M. A port forced to "100M full duplex" makes the adapter
  fall back to half duplex (duplex mismatch: errors and lost packets). If the switch can only force a fixed
  mode, use 100M half duplex or leave it on auto.

### IOS state and backups

The main screen shows every IOS: `stock` (signed by Nintendo), `MODIFIED` (installed by a patcher, this one
too) or `custom` (changed by another tool, e.g. the WiiLink IOS31 / IOS80 on a Wii). `*` means that no
backup on the SD card matches it.

- A stock IOS is saved as `IOSxx-64-vYYYY.wad`, a custom one as `IOSxx-64-vYYYY-custom.wad`. A MODIFIED IOS
  is never dumped, so a backup is never overwritten by a patched IOS.
- The backup is chosen by the content hashes of its TMD, not by the file name: it has to be identical to
  the IOS on the NAND, or differ only in the ETH module. Backups in `sd:/wad/` from older versions are
  still found.
- A custom IOS (WiiLink) keeps its changes when it is patched, only the ETH module is replaced.
- A patched IOS supports only the AX88179 / AX88179A.

The patcher writes a short log to `sd:/ethernet_patcher.log` (start, dump, patch, install and restore
results). Please add it to bug reports.

## Supported adapters

| Adapter | USB PID | Status |
|---------|---------|--------|
| AX88772  | 0x7720 | works with the stock IOS |
| AX88772B | 0x772B | supported |
| AX88772C | 0x772B | supported |
| AX88772D | 0x1790 | supported |
| AX88179 (Nintendo Switch LAN adapter) | 0x1790 | all 20 IOS, 10/100M |
| AX88179A | 0x1790 | all 20 IOS (CDC-ECM mode), limit the port to 100M |

The AX88179 family is recognized by its USB product string ("AX88179" / "AX88179A").

### Tested with the AX88179 / AX88179A

| IOS | Used by | Tested |
|-----|---------|--------|
| IOS58 | homebrew | Wii U: AX88179 and AX88179A (DHCP, TCP, UDP up to 1472 bytes). Wii: AX88179 |
| IOS36 | Mario Kart Wii and many other games | Wii U: AX88179 and AX88179A, Mario Kart Wii (CTGP-R) races on Wiimmfi. Wii: AX88179 |
| IOS31, 33, 34, 35, 38 | games, IOS31: WiiConnect24 on a Wii | same module and kernel as IOS36; Wii (WiiLink): IOS31 with the AX88179 |
| IOS80 | System Menu | vWii with Priiloader: connection test. Wii (WiiLink): connection test, Message Board mail, link stable for days in WiiConnect24 standby |
| IOS53, 56, 57, 59, 62 | games | Wii U: network test passed |
| IOS37, 41, 43, 45, 46, 48, 55 | games | same module as IOS36, not tested on hardware yet |

## Building

Requires [devkitPPC](https://devkitpro.org/) with libogc and libfat.

**Use libogc 3.1.0.** With libogc 3.2.1 the program starts and patches fine, but [B] Test network crashes
(in `net_init()`, seen on a Wii). The cause is not found yet. On Arch with devkitPro-pacman:
`sudo downgrade libogc` (pick 3.1.0) and keep it in `IgnorePkg`.

    make clean && make

New to the code? [HOW_IT_WORKS.md](HOW_IT_WORKS.md) explains the terms (IOS, TMD, WAD, ...), what each menu
entry does and which file does what.

The AX88179 patch data in `source/ax179_*_data.h` is generated from the stock modules; the driver sources,
the generator scripts and the emulator tests are in `tools/ax179/` (see its README). You do not need that folder to build or use the patcher.

## Origin of the files

The patcher is based on doworian's vWii Ethernet Patcher. Its files are kept in his style, so the
changes in them stay easy to compare with the original. Everything that was added later is in new
files. Paths are relative to `source/`.

**Unchanged from the original** (byte-identical):

| File | What it does |
|------|--------------|
| `patch_772bc.c/h`, `patch_772d.c/h`, `patch_common.h` | AX88772B / C / D patches |
| `identify.h`, `isfs.h`, `wad.h` | headers |
| `sha1.c/h`, `rijndael.c/h` | SHA-1 (Steve Reid) and AES (Mike Scott) |
| `memory/` | MEM2 allocator (from WiiFlow) |

**Original, with small fixes** (the original code and style are kept):

| File | Changed |
|------|---------|
| `IOSPatcher.c/h` | the original ticket is put back after an install (vWii key index 2); TMD completeness check; backups are looked for in `sd:/wad/<wii\|vwii>/` first, then in the old `sd:/wad/` |
| `isfs.c` | only complete reads from the NAND count; shared contents always come from `/shared1`; log messages |
| `wad.c` | a short or broken WAD file is an error instead of being read past its end |
| `identify.c` | the vWii-only patterns are only searched on the vWii |
| `tools.c/h` | log file `sd:/ethernet_patcher.log` |
| `makefile` | file name `makefile` instead of `Makefile` |

**Rewritten:**

| File | What it does |
|------|--------------|
| `main.c` | all menus. Based on the original `main.c`, but rewritten for the new menu entries (dump, restore, AX88179 IOS list, network test); the AX88772 path works as in the original |

**New:**

| File | What it does |
|------|--------------|
| `patch_179.c/h`, `ax179_*_data.h` | AX88179 / AX88179A patch (the data files are generated by `tools/ax179/`) |
| `backup.c/h` | names of the backup files, finds the backup that matches the NAND |
| `sigcheck.c/h` | checks the Nintendo signature of a TMD: stock, MODIFIED or custom |
| `platform.c/h` | Wii or vWii, list of the IOS of the console |
| `tools/ax179/` (outside `source/`) | driver sources, patch generators and emulator tests |
| `meta.xml`, `HOW_IT_WORKS.md` | Homebrew Channel entry, short tour through the code |

## Credits

Original vWii Ethernet Patcher by doworian (the repository was deleted; the author agreed to a GPL
publication). AX88179 / AX88179A support and the Wii port were added later.

Register analysis from the [Linux kernel asix drivers](https://github.com/torvalds/linux/tree/master/drivers/net/usb)
and SDIO's [wafel_ax88772b](https://github.com/StroopwafelCFW/wafel_ax88772b). IOS patches of the running
IOS from postloader (stfour). MEM2 allocator from WiiFlow, AES by Mike Scott, SHA-1 by Steve Reid.
