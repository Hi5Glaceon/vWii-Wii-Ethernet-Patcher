#!/usr/bin/env python3
"""Builds the AX88179 patch for one of the three Thumb ETH modules.

  58  IOS58 v6432, content #17 (file 00000011.app)
  57  IOS57 v6175, content #17 (file 00000011.app)
  59  IOS59 v9249, content #19 (file 00000013.app)

All three modules have the same layout. Only IOS59 has two more syscall stubs, so its syscall
veneers (IOB push / pull) sit 16 bytes further up.

What this script does:
  1. compiles ax179_drv.c (the new driver) and links it to the addresses of the old driver
  2. checks that the input is really the stock module (SHA-1)
  3. makes a list of "patch records": at file offset X, replace the old bytes with these new bytes
       - the new driver code replaces the old driver
       - 13 calls of the module are pointed at the new driver
       - the USB ID the module looks for is changed from AX88772 to AX88179
  4. applies the records to a copy of the module and writes the result

Usage:  build_patch.py <58|57|59> <stock ETH module>      (run in tools/ax179)

Needs: arm-none-eabi-gcc, arm-none-eabi-ld, arm-none-eabi-objcopy, arm-none-eabi-nm.

Writes:
  eth<ios>_patched.app     the patched module (for looking at it and for the tests)
  patch_data_<ios>.json    all patch records (read by test_driver.py)
  ax179_ios<ios>_data.h    the same records as C arrays, used by source/patch_179.c

GIGABIT=1 in the environment keeps 1000BASE-T advertised (default: only 10/100).
"""
import hashlib, json, os, struct, subprocess, sys

# ---- what we know about the three modules ----------------------------------------------------
# ios: (version of the IOS, number of the content, SHA-1 of the stock module, address of the
#       syscall veneers)
TARGETS = {
    58: (6432, 17, 'cc776639562ba298d66598efe0340b4255bf4b6c', 0x13aa2a68),
    57: (6175, 17, '60407c212848e0923829073c9558c027b378d8e2', 0x13aa2a68),
    59: (9249, 19, '7334677445c8013d1932149d387d4259e1986ea1', 0x13aa2a78),
}

IOS = int(sys.argv[1])
VERSION, CONTENT, ORIG_SHA1, IOB_VENEER = TARGETS[IOS]
ORIG = sys.argv[2]

# The module is loaded to memory address 0x13aa0000. Its code starts at byte 0x108 of the file.
FILE_OFFSET, LOAD_ADDRESS = 0x108, 0x13aa0000

# The old AX88772 driver is replaced by the new one. This is the space (in memory addresses):
BLOCK1_START, BLOCK1_END = 0x13aa0da4, 0x13aa1810
# The stock "stop" routine is also replaced (its only caller is pointed at the new code).
BLOCK2_START, BLOCK2_END = 0x13aa18cc, 0x13aa1960

PREFIX = f'ax179_ios{IOS}'      # name of the generated files and C variables

# compiler options: ARM926EJ-S in Thumb mode, big-endian, no standard library
COMPILER_OPTIONS = ('-mcpu=arm926ej-s -mthumb -mbig-endian -mfloat-abi=soft -Os -ffreestanding -fno-builtin '
                     '-nostdlib -fno-pic -fno-stack-protector -mno-unaligned-access -fno-unwind-tables '
                     '-fno-asynchronous-unwind-tables -Wall -Wextra').split()

# The 13 calls in the module that go to the old driver. The new driver replaces them.
# (address of the call, address the call goes to in the stock module, name of the new function)
CALLS = [
    (0x13aa00ce, 0x13aa1234, 'ax179_rx'),
    (0x13aa0380, 0x13aa14f8, 'ax179_init'),
    (0x13aa038e, 0x13aa1068, 'ax179_rxmode'),
    (0x13aa039c, 0x13aa1328, 'ax179_rxreset'),
    (0x13aa03ec, 0x13aa18cc, 'ax179_stop'),
    (0x13aa0446, 0x13aa1184, 'ax179_link'),
    (0x13aa0478, 0x13aa14e4, 'ax179_handles'),
    (0x13aa0492, 0x13aa14f8, 'ax179_init'),
    (0x13aa09d0, 0x13aa102c, 'ax179_setmac'),
    (0x13aa0bd0, 0x13aa0fec, 'ax179_getmac'),
    (0x13aa0c20, 0x13aa1068, 'ax179_rxmode'),
    (0x13aa0d0c, 0x13aa11cc, 'ax179_tx'),
    (0x13aa187e, 0x13aa0f1c, 'ax179_swreset'),
]

# The module compares the USB ID of the adapter with 0x0B957720 (ASIX AX88772). The AX88179 has
# the ID 0x0B951790. The bytes of the number are found in the module together with the padding
# behind it. Only the last two bytes of the ID (7720 -> 1790) are changed.
USB_ID_PATTERN = bytes.fromhex('0b95772000ffffff')
USB_ID_OLD = b'\x77\x20'
USB_ID_NEW = b'\x17\x90'


def run(command):
    subprocess.check_call(command)


def file_offset(address):
    """Memory address -> position in the module file."""
    return address - LOAD_ADDRESS + FILE_OFFSET


def make_bl(from_address, to_address):
    """The 4 bytes of a Thumb 'BL' instruction (a call) at from_address that jumps to to_address."""
    distance = to_address - (from_address + 4)
    assert -(1 << 22) <= distance < (1 << 22) and distance % 2 == 0
    distance &= 0x7fffff
    high_part = 0xF000 | (distance >> 12)
    low_part = 0xF800 | ((distance >> 1) & 0x7ff)
    return struct.pack('>HH', high_part, low_part)


def read_bl_target(module, from_address):
    """Where does the Thumb 'BL' instruction at from_address jump to?"""
    first, second = struct.unpack('>HH', module[file_offset(from_address):file_offset(from_address) + 4])
    assert (first & 0xF800) == 0xF000 and (second & 0xF800) == 0xF800, hex(from_address)
    distance = ((first & 0x7ff) << 12) | ((second & 0x7ff) << 1)
    if distance & 0x400000:
        distance -= 0x800000
    return from_address + 4 + distance


def fill_with_undefined(code, size):
    """Pads code to size bytes with the Thumb instruction 'undefined', so that a stray jump traps."""
    padding = b'\xde\x00' * ((size - len(code)) // 2)
    return code + padding


# ---- step 1: compile and link the new driver --------------------------------------------------
run(['arm-none-eabi-gcc', *COMPILER_OPTIONS, '-DGIGABIT=' + os.environ.get('GIGABIT', '0'),
     f'-DIOB_VENEER={IOB_VENEER:#x}',
     '-c', 'ax179_drv.c', '-o', 'drv.o'])
run(['arm-none-eabi-ld', '-EB', '-T', 'block.ld', '-o', 'drv.elf', 'drv.o'])
run(['arm-none-eabi-objcopy', '-O', 'binary', '-j', '.blk', 'drv.elf', 'drv.bin'])
run(['arm-none-eabi-objcopy', '-O', 'binary', '-j', '.blk2', 'drv.elf', 'drv2.bin'])
code1 = open('drv.bin', 'rb').read()      # the driver
code2 = open('drv2.bin', 'rb').read()     # the new stop routine

# the addresses of the new functions (name -> address)
symbols = {}
for line in subprocess.check_output(['arm-none-eabi-nm', 'drv.elf'], text=True).splitlines():
    address, kind, name = line.split()
    symbols[name] = int(address, 16)
assert len(code1) <= BLOCK1_END - BLOCK1_START, 'block too large'
assert len(code2) <= BLOCK2_END - BLOCK2_START, 'block 2 too large'

# ---- step 2: read the stock module and check it ----------------------------------------------
module = bytearray(open(ORIG, 'rb').read())
assert hashlib.sha1(module).hexdigest() == ORIG_SHA1, 'unexpected input module'

# ---- step 3: make the patch records -----------------------------------------------------------
# A record is (file offset, old bytes, new bytes).
patches = []

# 3a) the new driver goes over the old one, the rest of the space is filled with 'undefined'
new_block1 = fill_with_undefined(code1, BLOCK1_END - BLOCK1_START)
assert len(new_block1) == BLOCK1_END - BLOCK1_START
old_block1 = bytes(module[file_offset(BLOCK1_START):file_offset(BLOCK1_END)])
patches.append((file_offset(BLOCK1_START), old_block1, new_block1))

new_block2 = fill_with_undefined(code2, BLOCK2_END - BLOCK2_START)
assert len(new_block2) == BLOCK2_END - BLOCK2_START
old_block2 = bytes(module[file_offset(BLOCK2_START):file_offset(BLOCK2_END)])
patches.append((file_offset(BLOCK2_START), old_block2, new_block2))

# 3b) point the calls at the new functions
for call_address, old_target, function_name in CALLS:
    assert read_bl_target(module, call_address) == old_target, (hex(call_address), hex(read_bl_target(module, call_address)), hex(old_target))
    old_bytes = bytes(module[file_offset(call_address):file_offset(call_address) + 4])
    new_bytes = make_bl(call_address, symbols[function_name])
    patches.append((file_offset(call_address), old_bytes, new_bytes))

# 3c) the USB ID (this is used by FUN_13aa19b0). It must be in the module exactly once.
position = module.find(USB_ID_PATTERN)
assert position >= 0 and module.find(USB_ID_PATTERN, position + 1) < 0
patches.append((position + 2, USB_ID_OLD, USB_ID_NEW))

# ---- step 4: apply the records and write the results -----------------------------------------
patched = bytearray(module)
for offset, old_bytes, new_bytes in patches:
    assert patched[offset:offset + len(old_bytes)] == old_bytes
    patched[offset:offset + len(new_bytes)] = new_bytes
open(f'eth{IOS}_patched.app', 'wb').write(patched)
new_sha1 = hashlib.sha1(patched).hexdigest()

# for the emulator test
json.dump({'patches': [(o, a.hex(), b.hex()) for o, a, b in patches],
           'syms': symbols, 'orig_sha1': ORIG_SHA1, 'new_sha1': new_sha1,
           'blob_len': len(code1), 'blob2_len': len(code2), 'iob_veneer': IOB_VENEER},
          open(f'patch_data_{IOS}.json', 'w'), indent=1)


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


NAME = PREFIX.upper()       # e.g. AX179_IOS58
gigabit = int(os.environ.get('GIGABIT', '0') == '1')

header = [f'/* generated by tools/ax179/build_patch.py {IOS} - do not edit */',
          f'#ifndef _{NAME}_DATA_H_', f'#define _{NAME}_DATA_H_', '',
          f'/* IOS{IOS} v{VERSION} ETH module (content #{CONTENT}). The stock module is identified by its SHA1,',
          ' * so the original bytes are not shipped here. */',
          f'#define {NAME}_MODULE_SIZE {len(module)}u',
          f'#define {NAME}_GIGABIT {gigabit}   /* 0: 10/100 only */',
          f'static const u8 {PREFIX}_orig_sha1[20] = {{ {c_bytes(bytes.fromhex(ORIG_SHA1))} }};',
          f'static const u8 {PREFIX}_new_sha1[20]  = {{ {c_bytes(bytes.fromhex(new_sha1))} }};', '']
for n, (offset, old_bytes, new_bytes) in enumerate(patches):
    header.append(f'static const u8 {PREFIX}_p{n}[{len(new_bytes)}] = {{\n{c_array_body(new_bytes)}\n}};')
header.append('')
header.append(f'static const ax179_patch_rec_t {PREFIX}_patches[] = {{')
for n, (offset, old_bytes, new_bytes) in enumerate(patches):
    header.append(f'    {{ 0x{offset:04X}, {len(new_bytes)}, {PREFIX}_p{n} }},')
header.append('};')
header.append(f'#define {NAME}_PATCH_COUNT (sizeof({PREFIX}_patches) / sizeof({PREFIX}_patches[0]))')
header.append('')
header.append('#endif')
open(f'{PREFIX}_data.h', 'w').write('\n'.join(header) + '\n')

print(f'driver block {len(code1)} / {BLOCK1_END - BLOCK1_START} bytes, block 2 {len(code2)} / {BLOCK2_END - BLOCK2_START} bytes, {len(patches)} patch records')
print('entry points:', {k: hex(v) for k, v in symbols.items() if k.startswith('ax179_')})
print('patched module sha1', new_sha1)
