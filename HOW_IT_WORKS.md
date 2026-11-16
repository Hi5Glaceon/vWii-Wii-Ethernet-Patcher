# How the patcher works

A short tour for anyone who wants to read or change the code. No Wii knowledge needed.

## The idea in one paragraph

Games and channels on the Wii do not talk to USB devices themselves. They use an **IOS**, a small
operating system that runs on a second processor (an ARM CPU, "Starlet"). Every IOS contains a module
for USB ethernet adapters, the **ETH module**, but it only knows the original AX88772 chip. This tool
takes the IOS from the console, replaces the adapter part of the ETH module and installs the changed
IOS again. Nothing else in the IOS is touched.

## Words you will meet

| Word | Meaning |
|------|---------|
| IOS (e.g. IOS36) | One of the system's operating systems. Every game chooses the IOS it runs on. IOS80 is the one of the System Menu. |
| vWii | The Wii mode of the Wii U. Same IOS, slightly different versions. |
| Title | Anything installed on the NAND: an IOS, a channel, the System Menu. |
| NAND | The internal flash memory of the console. |
| Content | One file of a title. The ETH module is one content of an IOS. |
| TMD | "Title metadata": the list of contents with their SHA-1 hashes, signed by Nintendo. |
| Ticket | Holds the (encrypted) key of a title. The contents are AES encrypted with it. |
| WAD | A file that packs a whole title: certificates, ticket, TMD and the encrypted contents. Backups are WADs. |
| Fakesign | A patched TMD can not get a real Nintendo signature. Its signature is set to zeros and a filler field is changed until the hash starts with 0. Patched IOS accept that. |
| cIOS | A "custom IOS" that a USB loader runs games on. The d2x installer builds it from a stock IOS (its "base") and installs it fakesigned into a free slot, usually 248 to 251. |
| stock / MODIFIED / custom / cIOS | What the patcher shows for an IOS: signed by Nintendo / fakesigned (patched) / changed by another tool with an invalid signature (e.g. WiiLink) / a cIOS that the patcher did not patch. |
| AHBPROT | Full hardware access for the program. The Homebrew Channel gives it to apps with `<ahb_access/>` in `meta.xml`. |

## What happens when you press a button

All menu code is in `main.c`, as in the original tool. The file is read from top to bottom in ten
numbered sections (listed at its top): small helpers, adapter detection, IOS state, WAD files, one
section per menu entry and the main screen with `main()` at the end.

**Start.** Shows a warning, gets AHBPROT, patches the running IOS in memory so that it accepts
fakesigned titles (`identify.c`), mounts the SD card, finds out if this is a Wii or a vWii and makes
the list of IOS to work with, with the installed cIOS at the end (`platform.c`, `cios.c`), and reads
the state of every IOS (`refresh_status()`).

**[+] Dump (`dump_all_ios()`).** Reads each IOS from the NAND (`isfs.c`), checks every content against
its TMD hash, encrypts it again and writes it as a WAD to `sd:/wad/wii/` or `sd:/wad/vwii/`.
A fakesigned (patched) IOS is never dumped, so a backup can not be overwritten by a patched IOS.
A cIOS is fakesigned too, but it is dumped (as `-cios.wad`): see "cIOS" below.

**[A] Patch.**
- *AX88179 / AX88179A (`patch_all_179()`):* you choose the IOS. For each one the patcher looks for the
  backup the installed IOS was made from (`backup.c` compares the content hashes of the TMDs), loads it
  (`wad.c`), replaces the driver in the ETH module (`patch_179.c`), fakesigns the TMD and writes the
  result to the SD card (dry run). After you confirm, the same is done again and installed (`IOSPatcher.c`).
- *AX88772B / C / D (`patch_all_772()`):* the patch of the original tool (`patch_772bc.c`,
  `patch_772d.c`), installed right away. It starts from the same backup as the AX88179 patch, so an IOS
  without a backup is not patched, and switching between the two drivers always starts from the stock IOS.
  It takes the cIOS of the list too (they carry the stock ETH module of their base).

**The AX88772B and C** have the same USB ID (0x772B) and differ by the device version (1.00 / 2.00). The
version is read from the opened device (`read_version()`); `USB_GetDeviceDescription()` needs the handle
of `USB_OpenDevice()`, not the device ID. Before 2.1 the device ID was passed, the read always failed and
every 772C was shown as 772B. Both use the same patch, so only the name on the screen was wrong.

**[B] Test network (`network_test()`).** Loads IOS58, starts the network and shows the local and the
public IP. Afterwards the patcher ends and returns to the Homebrew Channel: the IOS network stays
running and takes the adapter away from the USB list after about a minute, so the menu could not use
it any more. Before the reload the SD card and ISFS are closed: libogc keeps their handles across a
reload, and in the new IOS the same handle numbers belong to other devices, so closing them later
(when leaving) could hit the wrong device and freeze the console. If the network does not start, the
start is stopped with `net_deinit()`, and the screen and the log name the adapter seen in the menu.
The network is started with `net_init_async()` and the status is polled: `net_init()` itself sleeps until
the IOS answers and would freeze the patcher if the IOS never does (seen with an AX88772C on an IOS58
that has the AX88179 driver). After 40 s without an answer the test stops, does not call `net_deinit()`
(it would wait for the same answer) and the IOS reload when leaving ends the stuck start.

**[-] Restore (`restore_stock_ios()`).** Every MODIFIED IOS gets its backup back. A cIOS that was not
patched is left alone. A backup file that is itself patched (dumped after patching) is never used;
`backup_find_patched_copy()` finds it so the screen can say what it is.

## How the AX88179 patch works

The new drivers are written in C (`tools/ax179/ax179_ios36.c` and `ax179_drv.c`) and compiled for the
ARM CPU of the IOS. A Python script puts the compiled code over the old AX88772 code inside the stock
ETH module and changes the calls that went to the old functions so that they go to the new ones. The
result is stored as a list of "write these bytes at this offset" in `source/ax179_*_data.h`, together
with the SHA-1 of the stock module and of the patched module.

`patch_179.c` only patches a module whose SHA-1 is exactly the expected stock one, and only accepts the
result if its SHA-1 is exactly the expected patched one. So a patch can never land on a module it was
not made for. Details: `tools/ax179/README.md`.

## cIOS

A cIOS has no Nintendo signature, so `sigcheck.c` alone would call it MODIFIED, the same as an IOS
this tool patched. `cios.c` looks at the content list of the TMD instead: if one of the contents has the size
of an ETH module but not the hash of a stock one, the ETH module was changed (AX88179 driver or AX88772B / C / D
patch, both work inside the module and keep its size) and the IOS is MODIFIED; otherwise it is a cIOS that
nobody patched. Without this, a cIOS patched for the AX88772 would look like an untouched one and a dump
would overwrite its backup. Only the hashes in the TMD are needed for that, not the contents.

From there a cIOS is handled like a custom IOS: dumped to its own backup file, patched from that backup
(only the ETH module is replaced, the d2x parts stay), and restored from it.

One thing a cIOS does differently: every content is AES encrypted with its *index* from the TMD as IV.
In a stock IOS the index is the same as the position in the list, but d2x lists some contents in
another order (in IOS249 the contents at position 3 and 17 have the index 17 and 3). So
`IOSPatcher.c` takes the index from the TMD (`content_index()`).

## Files

Which files come from the original tool and what was changed in them: see "Origin of the files" in
the [README](README.md).

| File | What it does |
|------|--------------|
| `main.c` | start, main screen, adapter detection and all menu entries |
| `IOSPatcher.c` | an IOS in memory: decrypt, encrypt, fakesign, install |
| `isfs.c` | reads an IOS from the NAND |
| `wad.c` | reads WAD files |
| `identify.c` | patches of the running IOS (in memory only) |
| `tools.c` | buttons, SD card, log file |
| `patch_772bc.c`, `patch_772d.c`, `patch_common.h` | AX88772B / C / D patches (unchanged from the original) |
| `patch_179.c` + `ax179_*_data.h` | AX88179 / AX88179A patch |
| `backup.c` | names of the backup files, finds the right backup |
| `sigcheck.c` | checks the Nintendo signature of a TMD (RSA) |
| `platform.c` | Wii or vWii, list of the IOS |
| `cios.c` | cIOS of USB loaders: the slots, cIOS or MODIFIED |
| `sha1.c`, `rijndael.c`, `memory/` | SHA-1, AES and the MEM2 allocator (from other projects) |

For the AX88179 and for [-] Restore nothing is written to the NAND before a confirmation with [1] [2] [A].
The AX88772 path installs after the [A] on its start screen, as the original tool did.
