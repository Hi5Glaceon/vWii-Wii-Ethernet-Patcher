# vWii Ethernet patcher

This project patches vWii IOS to support AX88772B, AX88772C, and AX88772D USB ethernet adapters.

The stock vWii driver only recognizes the original AX88772. Newer revisions use different product IDs and have register changes that break the stock driver. This patches all 20 IOS titles that contain the ethernet driver.

## Prerequisites

- vWii with Homebrew Channel (Required)
- SD card (Required)
- Priiloader + Aroma (Recommended)

## How to use

1. Extract to your SD card so you have `sd:/apps/vwii_ethernet_patcher/boot.dol` and `meta.xml`
2. Launch from Homebrew Channel
3. Plug in your adapter
4. Press [+] to dump stock IOS to SD first (saves to `sd:/wad/`)
5. Press [A] to patch (only available when a supported adapter is detected)
6. Press [B] to test the network after patching
7. Press [HOME] to exit

The patcher reads from your dumped WADs on SD, not directly from NAND. Dump first, then patch.

## Supported adapters

| Adapter | USB PID | Status |
|---------|---------|--------|
| AX88772  | 0x7720 | stock. |
| AX88772B | 0x772B | supported |
| AX88772C | 0x772B | supported |
| AX88772D | 0x1790 | supported |

## Building

Requires [devkitPPC](https://devkitpro.org/) with libogc and libfat.

```
make clean && make
```

## Credits

Register analysis from the [linux kernel asix driver](https://github.com/torvalds/linux/tree/master/drivers/net/usb) and SDIO's [wafel_ax88772b](https://github.com/StroopwafelCFW/wafel_ax88772b).
