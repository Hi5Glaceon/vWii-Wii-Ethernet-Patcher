#!/usr/bin/env python3
"""Builds the AX88179 patch for the ETH module of IOS36 v3864 (content 0x0a, file 0000000a.app).

This module is ARM code. The new driver (ax179_ios36.c) is Thumb code. The same module is also
in IOS31, 33, 34, 35, 37, 38, 41, 43, 45, 46, 48, 53, 55, 56, 62 and 80.

What this script does:
  1. compiles ax179_ios36.c (the new driver) and links it to the addresses of the old driver
  2. checks that the input is really the stock module (SHA-1)
  3. makes a list of "patch records": at file offset X, replace the old bytes with these new bytes
       - the new driver code replaces the old driver
       - 11 calls of the module are pointed at the new driver
       - the USB ID the module looks for is changed from AX88772 to AX88179
  4. applies the records to a copy of the module and writes the result

Usage:  build_patch36.py <stock 0000000a.app> [output folder]

Needs: arm-none-eabi-gcc, arm-none-eabi-ld, arm-none-eabi-objcopy, arm-none-eabi-nm, arm-none-eabi-objdump.

Writes (into the output folder, default: this folder):
  0000000a_patched.app    the patched module (for looking at it and for the tests)
  patch36.json            all patch records (read by test36.py)
  ax179_ios36_data.h      the same records as C arrays, used by source/patch_179.c

GIGABIT=1 in the environment keeps 1000BASE-T advertised (default: only 10/100).
"""
import hashlib, json, os, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ORIG = sys.argv[1]
OUT = sys.argv[2] if len(sys.argv) > 2 else HERE

# The module is loaded to memory address 0x13aa0000. Its code starts at byte 0x128 of the file.
FILE_OFFSET, LOAD_ADDRESS = 0x128, 0x13aa0000
ORIG_SHA1 = 'aa6b000147dd12c838e40e58b25d3f5f510585f8'      # the stock module

# The old AX88772 driver is replaced by the new one. These are the two spaces (memory addresses):
BLOCK1 = (0x13aa0084, 0x13aa1290)     # chip logic up to axInit (axOpen at 0x13aa1290 stays)
BLOCK2 = (0x13aa13c8, 0x13aa1500)     # axForceReadCompletion + axDown (axClose at 0x13aa1500 stays)

# What is written into the space that the new driver does not need: a 4 byte ARM instruction that
# is always 'undefined', so that a stray jump traps instead of running garbage.
UNDEFINED = bytes.fromhex('e7f000f0')

# compiler options: ARM926EJ-S in Thumb mode, big-endian, no standard library
COMPILER_OPTIONS = ('-mcpu=arm926ej-s -mthumb -mbig-endian -mfloat-abi=soft -Os -ffreestanding -fno-builtin '
                     '-nostdlib -fno-pic -fno-stack-protector -mno-unaligned-access -fno-unwind-tables '
                     '-fno-asynchronous-unwind-tables -fno-tree-loop-distribute-patterns -Wall -Wextra -Werror').split()

# The 11 calls in the module that go to the old driver. The new driver replaces them.
# (address of the call, address the call goes to in the stock module, name of the new function)
CALLS = [
    (0x13aa195c, 0x13aa0a54, 'ax179_rx'),
    (0x13aa1d18, 0x13aa13c8, 'ax179_forceread'),
    (0x13aa1fe4, 0x13aa07d8, 'ax179_rxmode'),
    (0x13aa2004, 0x13aa0c70, 'ax179_up'),
    (0x13aa20a4, 0x13aa1410, 'ax179_down'),
    (0x13aa211c, 0x13aa0914, 'ax179_link'),
    (0x13aa27b4, 0x13aa0e7c, 'ax179_init'),
    (0x13aa3018, 0x13aa0758, 'ax179_getmac'),
    (0x13aa3088, 0x13aa0798, 'ax179_setmac'),
    (0x13aa3180, 0x13aa07d8, 'ax179_rxmode'),
    (0x13aa3220, 0x13aa097c, 'ax179_tx'),
]


def run(*command):
    subprocess.check_call(command, cwd=OUT)


def file_offset(address):
    """Memory address -> position in the module file."""
    return address - LOAD_ADDRESS + FILE_OFFSET


def make_bl(from_address, to_address, condition):
    """The 4 bytes of an ARM call at from_address that jumps to to_address.
    A normal ARM call is 'BL'. If to_address is Thumb code (bit 0 is set) it is 'BLX'."""
    if to_address & 1:
        assert condition == 0xE                  # BLX has no condition: it must be 'always'
        distance = (to_address & ~1) - (from_address + 8)
        assert -(1 << 25) <= distance < (1 << 25)
        half_bit = ((distance >> 1) & 1) << 24   # the call can reach 2-byte aligned (Thumb) addresses
        return struct.pack('>I', 0xFA000000 | half_bit | ((distance >> 2) & 0xFFFFFF))
    distance = (to_address - (from_address + 8)) >> 2
    assert -(1 << 23) <= distance < (1 << 23)
    return struct.pack('>I', (condition << 28) | 0x0B000000 | (distance & 0xFFFFFF))


def read_bl_target(module, from_address):
    """Where does the ARM 'BL' at from_address jump to? Returns (address, condition)."""
    word = struct.unpack('>I', module[file_offset(from_address):file_offset(from_address) + 4])[0]
    assert (word & 0x0F000000) == 0x0B000000, hex(from_address)
    distance = word & 0xFFFFFF
    if distance & 0x800000:
        distance -= 0x1000000
    return (from_address + 8 + distance * 4) & 0xffffffff, word >> 28


# ---- step 1: compile and link the new driver --------------------------------------------------
source = os.path.join(HERE, 'ax179_ios36.c')
linker_script = os.path.join(HERE, 'block36.ld')
run('arm-none-eabi-gcc', *COMPILER_OPTIONS, '-DGIGABIT=' + os.environ.get('GIGABIT', '0'),
    '-c', source, '-o', 'drv36.o')
run('arm-none-eabi-ld', '-EB', '-T', linker_script, '-o', 'drv36.elf', 'drv36.o')
run('arm-none-eabi-objcopy', '-O', 'binary', '-j', '.blk', 'drv36.elf', 'drv36.bin')
run('arm-none-eabi-objcopy', '-O', 'binary', '-j', '.blk2', 'drv36.elf', 'drv36b.bin')
code1 = open(os.path.join(OUT, 'drv36.bin'), 'rb').read()       # the driver
code2 = open(os.path.join(OUT, 'drv36b.bin'), 'rb').read()      # second block (empty at the moment)
assert len(code2) <= BLOCK2[1] - BLOCK2[0], 'second block too large'

# the addresses of the new functions (name -> address). Bit 0 is set for Thumb code.
symbols = {}
for line in subprocess.check_output(['arm-none-eabi-nm', 'drv36.elf'], cwd=OUT, text=True).splitlines():
    address, kind, name = line.split()
    symbols[name] = int(address, 16) | (1 if kind in 'Tt' else 0)
assert len(code1) <= BLOCK1[1] - BLOCK1[0], 'driver does not fit'

# The code must not need writable memory (there is none): no .data / .bss / .rodata in the object.
for line in subprocess.check_output(['arm-none-eabi-objdump', '-h', 'drv36.o'], cwd=OUT, text=True).splitlines():
    parts = line.split()
    if len(parts) > 2 and parts[1] in ('.data', '.bss', '.rodata') and int(parts[2], 16):
        sys.exit(f'unexpected {parts[1]} section - code must not need writable data')

# ---- step 2: read the stock module and check it ----------------------------------------------
module = bytearray(open(ORIG, 'rb').read())
assert hashlib.sha1(module).hexdigest() == ORIG_SHA1, 'not the stock IOS36 v3864 ETH module'

# ---- step 3: make the patch records -----------------------------------------------------------
# A record is (file offset, old bytes, new bytes).
patches = []


def add_record(offset, new_bytes):
    """Adds a record. The old bytes are taken from the stock module."""
    old_bytes = bytes(module[offset:offset + len(new_bytes)])
    patches.append((offset, old_bytes, bytes(new_bytes)))


# 3a) the new driver goes over the old one, the rest of the space is filled with 'undefined'
add_record(file_offset(BLOCK1[0]), code1 + UNDEFINED * ((BLOCK1[1] - BLOCK1[0] - len(code1)) // 4))
add_record(file_offset(BLOCK2[0]), code2 + UNDEFINED * ((BLOCK2[1] - BLOCK2[0] - len(code2)) // 4))
assert len(patches[0][2]) == BLOCK1[1] - BLOCK1[0]

# 3b) point the calls at the new functions
for call_address, old_target, function_name in CALLS:
    target, condition = read_bl_target(module, call_address)
    assert target == old_target and condition == 0xE, (hex(call_address), hex(target), condition)
    add_record(file_offset(call_address), make_bl(call_address, symbols[function_name], condition))

# 3c) the USB ID 0x0B95:0x7720 (AX88772) becomes 0x0B95:0x1790 (AX88179). It is written in four places.
# The product ID is built by two instructions: MOV r3,#0x7700 ; ADD r3,r3,#0x20  ->  0x1700 + 0x90 = 0x1790
assert module[file_offset(0x13aa22bc):file_offset(0x13aa22bc) + 4] == bytes.fromhex('e3a03c77')
assert module[file_offset(0x13aa22cc):file_offset(0x13aa22cc) + 4] == bytes.fromhex('e2833020')
add_record(file_offset(0x13aa22bc), bytes.fromhex('e3a03c17'))
add_record(file_offset(0x13aa22cc), bytes.fromhex('e2833090'))
# The device list search (0x13aa1ea8) builds vendor << 16 | product from two additions:
#   0x0b900000 + 0x57000 + 0x720 = 0x0b957720   ->   0x0b900000 + 0x51000 + 0x790 = 0x0b951790
assert module[file_offset(0x13aa1ec0):file_offset(0x13aa1ec0) + 4] == bytes.fromhex('e28cca57')
assert module[file_offset(0x13aa1ec4):file_offset(0x13aa1ec4) + 4] == bytes.fromhex('e28cce72')
add_record(file_offset(0x13aa1ec0), bytes.fromhex('e28cca51'))
add_record(file_offset(0x13aa1ec4), bytes.fromhex('e28cce79'))
# The two device paths /dev/usb/ehc/0b95/7720 and /dev/usb/oh0/0b95/7720 (each exactly once).
for path in (b'/dev/usb/ehc/0b95/7720\0', b'/dev/usb/oh0/0b95/7720\0'):
    position = module.find(path)
    assert position >= 0 and module.find(path, position + 1) < 0, path
    add_record(position + 18, b'1790')

# ---- step 4: apply the records and write the results -----------------------------------------
patched = bytearray(module)
for offset, old_bytes, new_bytes in patches:
    assert patched[offset:offset + len(old_bytes)] == old_bytes
    patched[offset:offset + len(new_bytes)] = new_bytes
open(os.path.join(OUT, '0000000a_patched.app'), 'wb').write(patched)
new_sha1 = hashlib.sha1(patched).hexdigest()

# for the emulator test
json.dump({'patches': [(o, a.hex(), b.hex()) for o, a, b in patches], 'syms': symbols,
           'orig_sha1': ORIG_SHA1, 'new_sha1': new_sha1, 'blob_len': len(code1), 'blob2_len': len(code2)},
          open(os.path.join(OUT, 'patch36.json'), 'w'), indent=1)


# ---- the C header for the PowerPC patcher (source/patch_179.c) --------------------------------
def c_array_body(data):
    """The bytes as C text: 16 per line, e.g.    0x01,0x02,..."""
    lines = []
    for i in range(0, len(data), 16):
        lines.append('    ' + ','.join(f'0x{x:02X}' for x in data[i:i + 16]))
    return ',\n'.join(lines)


def c_bytes(data):
    """The bytes as C text on one line, e.g. 0x01, 0x02"""
    return ', '.join(f'0x{x:02X}' for x in data)


gigabit = int(os.environ.get('GIGABIT', '0') == '1')

header = ['/* generated by tools/ax179/build_patch36.py - do not edit */',
          '#ifndef _AX179_IOS36_DATA_H_', '#define _AX179_IOS36_DATA_H_', '',
          f'#define AX179_IOS36_MODULE_SIZE {len(module)}u',
          f'#define AX179_IOS36_GIGABIT {gigabit}   /* 0: 10/100 only */',
          f'static const u8 ax179_ios36_orig_sha1[20] = {{ {c_bytes(bytes.fromhex(ORIG_SHA1))} }};',
          f'static const u8 ax179_ios36_new_sha1[20]  = {{ {c_bytes(bytes.fromhex(new_sha1))} }};', '']
for n, (offset, old_bytes, new_bytes) in enumerate(patches):
    header.append(f'static const u8 ax179_ios36_p{n}[{len(new_bytes)}] = {{\n{c_array_body(new_bytes)}\n}};')
header += ['', '/* offsets in the decrypted module; the stock module is identified by its SHA1, so the',
           ' * original bytes are not shipped here */',
           'static const ax179_patch_rec_t ax179_ios36_patches[] = {']
for n, (offset, old_bytes, new_bytes) in enumerate(patches):
    header.append(f'    {{ 0x{offset:04X}, {len(new_bytes)}, ax179_ios36_p{n} }},')
header += ['};', '#define AX179_IOS36_PATCH_COUNT (sizeof(ax179_ios36_patches) / sizeof(ax179_ios36_patches[0]))', '', '#endif']
open(os.path.join(OUT, 'ax179_ios36_data.h'), 'w').write('\n'.join(header) + '\n')

changed_bytes = sum(len(new_bytes) for _, _, new_bytes in patches)
print(f'driver {len(code1)} / {BLOCK1[1] - BLOCK1[0]} bytes, second block {len(code2)} / {BLOCK2[1] - BLOCK2[0]}, '
      f'{len(patches)} patch records, {changed_bytes} bytes changed region-wise')
print('patched module sha1', new_sha1)
