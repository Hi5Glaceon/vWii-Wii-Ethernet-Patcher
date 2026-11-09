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
| stock / MODIFIED / custom | What the patcher shows for an IOS: signed by Nintendo / fakesigned (patched) / changed by another tool with an invalid signature (e.g. WiiLink). |
| AHBPROT | Full hardware access for the program. The Homebrew Channel gives it to apps with `<ahb_access/>` in `meta.xml`. |

## What happens when you press a button

All menu code is in `main.c`, as in the original tool. The file is read from top to bottom in ten
numbered sections (listed at its top): small helpers, adapter detection, IOS state, WAD files, one
section per menu entry and the main screen with `main()` at the end.

**Start.** Shows a warning, gets AHBPROT, patches the running IOS in memory so that it accepts
fakesigned titles (`identify.c`), mounts the SD card, finds out if this is a Wii or a vWii
(`platform.c`) and reads the state of every IOS (`refresh_status()`).

**[+] Dump (`dump_all_ios()`).** Reads each IOS from the NAND (`isfs.c`), checks every content against
its TMD hash, encrypts it again and writes it as a WAD to `sd:/wad/wii/` or `sd:/wad/vwii/`.
A fakesigned (patched) IOS is never dumped, so a backup can not be overwritten by a patched IOS.

**[A] Patch.**
- *AX88179 / AX88179A (`patch_all_179()`):* you choose the IOS. For each one the patcher looks for the
  backup the installed IOS was made from (`backup.c` compares the content hashes of the TMDs), loads it
  (`wad.c`), replaces the driver in the ETH module (`patch_179.c`), fakesigns the TMD and writes the
  result to the SD card (dry run). After you confirm, the same is done again and installed (`IOSPatcher.c`).
- *AX88772B / C / D (`patch_all_772()`):* the original way of this tool: patch and install right away
  (`patch_772bc.c`, `patch_772d.c`).

**[B] Test network (`network_test()`).** Loads IOS58, starts the network and shows the local and the
public IP. Afterwards the patcher ends and returns to the Homebrew Channel: the IOS network stays
running and takes the adapter away from the USB list after about a minute, so the menu could not use
it any more.

**[-] Restore (`restore_stock_ios()`).** Every fakesigned IOS gets its backup back.

## How the AX88179 patch works

The new drivers are written in C (`tools/ax179/ax179_ios36.c` and `ax179_drv.c`) and compiled for the
ARM CPU of the IOS. A Python script puts the compiled code over the old AX88772 code inside the stock
ETH module and changes the calls that went to the old functions so that they go to the new ones. The
result is stored as a list of "write these bytes at this offset" in `source/ax179_*_data.h`, together
with the SHA-1 of the stock module and of the patched module.

`patch_179.c` only patches a module whose SHA-1 is exactly the expected stock one, and only accepts the
result if its SHA-1 is exactly the expected patched one. So a patch can never land on a module it was
not made for. Details: `tools/ax179/README.md`.

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
| `sha1.c`, `rijndael.c`, `memory/` | SHA-1, AES and the MEM2 allocator (from other projects) |

For the AX88179 and for [-] Restore nothing is written to the NAND before a confirmation with [1] [2] [A].
The AX88772 path installs after the [A] on its start screen, as the original tool did.
