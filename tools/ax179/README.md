# tools/ax179 – the driver sources and how the patch data is made

**You do not need anything from this folder to use or build the patcher.**
The finished result of this folder is already in `source/` (the four `ax179_ios*_data.h` files).
This folder only exists so that anybody can see where those files come from, check them, or change the driver.

## The idea in four sentences

1. An IOS contains a small program, the *ETH module*, that talks to the USB network adapter.
   Nintendo's version only knows one adapter, the AX88772.
2. I wrote a new driver for the AX88179 (Nintendo Switch LAN adapter) and the AX88179A in C.
3. A Python script compiles it and writes the new bytes over the old driver inside the ETH module.
   The result is a list of "patch records": *at position X write these bytes*.
4. The patcher (`source/patch_179.c`) does exactly this list on the console. It only touches a module whose
   SHA-1 is exactly the one of Nintendo's original, and it checks the SHA-1 of the result.

Because of point 4 the data files contain **only the new bytes**, never any of Nintendo's code.

## The files

| File | What it is |
|------|------------|
| `ax179_drv.c`, `block.ld` | the new driver for the ETH module of **IOS58, IOS57 and IOS59** (Thumb code) |
| `ax179_ios36.c`, `block36.ld` | the new driver for the ETH module of **IOS36** and the 16 other IOS that carry the same module (ARM code with a Thumb driver) |
| `build_patch.py` | builds the patch for IOS58 / 57 / 59 and writes `ax179_ios58_data.h` etc. |
| `build_patch36.py` | builds the patch for the IOS36 module and writes `ax179_ios36_data.h` |
| `test_driver.py` | test for the IOS58 / 57 / 59 patch (runs the patched module in a CPU emulator) |
| `test36.py` | test for the IOS36 patch (the same idea) |

The `.ld` files are linker scripts. They only say at which memory address the new code has to be placed.
The comments at the top of each `.c` and `.py` file explain what the file does in more detail.

## What the two drivers do

* **AX88179** is driven through its vendor registers. The sequence of register writes follows a capture
  (usbmon) of the Linux driver.
* **AX88179A** does not come up in its vendor mode on the Wii, so the driver switches it to the standard
  CDC-ECM mode (USB configuration 3) and sends and receives plain ethernet frames. The driver recognises
  it because it has 3 USB configurations (the AX88179 has 1). Its MAC address is read from USB string 5.
* Both drivers keep the way Nintendo's code calls a driver. Each function of the new driver replaces one
  function of the old one (the comment above it says which), and the script only changes the places where the
  module calls these functions.

### Only 10/100M

Both drivers switch off 1000BASE-T (PHY register 9). On a Wii in WiiConnect24 standby the AX88179 came up at
1000M for a moment, dropped and settled at 100M; until then the System Menu's connection test needed 3 - 4
attempts and the Wii Message Board failed with error 51420. At a stable 100M everything worked, and the Wii
(USB 2.0) gains nothing from 1000M anyway. `GIGABIT=1` in the environment of the build scripts keeps 1000M.

The AX88179A negotiates its speed on its own in ECM mode, the driver cannot change it. So the switch / router
port of the console has to be limited to 100M (auto-negotiation limited to 100M, not a forced "100M full
duplex"). The patcher shows this hint when an AX88179A is plugged in.

### Not done

* ECM mode (AX88179A) has no detection of a lost link.
* In the IOS36 module a pending receive is not cancelled when the stack stops the receive thread; it returns
  with the next frame.

## Rebuilding the patch data (optional)

You need:

* `gcc-arm-none-eabi` and `binutils-arm-none-eabi` (the build was checked with gcc 13.2.1)
* Python 3 and `pip install unicorn pycryptodome` (unicorn for the tests, pycryptodome only if you want to
  decrypt the modules from WAD files yourself)
* the *original* ETH module of each IOS as a file. It is one content of the IOS. Take it from a WAD backup of
  your own console (the patcher makes these in `sd:/wad/`) and decrypt it. The scripts check the SHA-1 and stop
  if the file is not the right one.

| IOS | content | file | SHA-1 of the original module |
|-----|---------|------|------------------------------|
| IOS36 v3864 (also in IOS31, 33, 34, 35, 37, 38, 41, 43, 45, 46, 48, 53, 55, 56, 62, 80) | `0x0a` (IOS80: `0x0c`) | `0000000a.app` | `aa6b000147dd12c838e40e58b25d3f5f510585f8` |
| IOS58 v6432 (also the Wii IOS58 v6176) | 17 | `00000011.app` | `cc776639562ba298d66598efe0340b4255bf4b6c` |
| IOS57 v6175 (also the Wii IOS57 v5919) | 17 | `00000011.app` | `60407c212848e0923829073c9558c027b378d8e2` |
| IOS59 v9249 | 19 | `00000013.app` | `7334677445c8013d1932149d387d4259e1986ea1` |

Then, in this folder:

    python3 build_patch36.py /path/to/0000000a.app out/        # writes out/ax179_ios36_data.h
    python3 build_patch.py 58 /path/to/IOS58/00000011.app      # writes ax179_ios58_data.h here
    python3 build_patch.py 57 /path/to/IOS57/00000011.app
    python3 build_patch.py 59 /path/to/IOS59/00000013.app

Copy the four `ax179_ios*_data.h` files to `source/` and build the patcher again.

**Check:** with the original modules of the vWii IOS and gcc 13.2.1 the four data files come out
**byte for byte identical** to the ones in `source/`. If you get a different file, the most likely reason is a
different compiler version.

## Running the tests (optional)

    python3 test36.py out/0000000a_patched.app out/patch36.json
    python3 test_driver.py 58          # also 57 and 59, in the same folder after build_patch.py

The tests do not need a console. They load the patched module into a CPU emulator and run the module's own
code against a simulated adapter: open, bring the interface up, link up and down (1000M / 100M / 10M), get and
set the MAC, stop the receive thread, take the interface down and up again, and send and receive generated
frames. This is done for the AX88179 and for the AX88179A (ECM). The last line is `ALL OK` if every check passed
(95 checks for IOS36, 74 for each of IOS58 / 57 / 59).

The emulator is not a real adapter. The tests show that the code does what it is meant to do, not that every
adapter and every network behaves the same way. That is what the test table in the main README is for.
