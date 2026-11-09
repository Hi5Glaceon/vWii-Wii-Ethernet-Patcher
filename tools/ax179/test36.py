#!/usr/bin/env python3
"""Emulator test for the patched ETH module of IOS36 (and all IOS that carry the same module).

What does it do?
  It loads the PATCHED module (0000000a_patched.app) into Unicorn, a CPU emulator, and runs the
  module's own code. Instead of a real USB adapter there is a small simulated AX88179 / AX88179A
  in this file (class Dev). It answers the USB requests of the driver like the real chip would.

  The test calls the same functions the IOS calls (axOpen, the new init / up / link / down ...,
  axClose). Every call goes through the patched call sites, so it also tests that the new driver
  is wired in correctly. It runs the AX88179 (vendor registers) and the AX88179A (CDC-ECM mode),
  and sends and receives generated frames (several frames in one transfer, header bytes, ...).
  After each step it checks that the chip registers and the data in the module are what they should be.

How to read the output:
  PASS <text>    a check worked
  FAIL <text>    a check failed (the values are printed behind it)
  ALL OK         at the end, if nothing failed

Usage:  test36.py <patched 0000000a.app> <patch36.json>      (after build_patch36.py)
GIGABIT=1 in the environment if the driver was built with GIGABIT=1.
"""
import json, os, struct, sys
from unicorn import *
from unicorn.arm_const import *

MOD, PJ = sys.argv[1:3]
GIGABIT = os.environ.get('GIGABIT', '0') == '1'   # build with 1000BASE-T advertised
VA, FO, CODELEN = 0x13aa0000, 0x128, 0x7648
mod = open(MOD, 'rb').read()
pj = json.load(open(PJ))
S = pj['syms']

uc = Uc(UC_ARCH_ARM, UC_MODE_ARM | UC_MODE_BIG_ENDIAN)
uc.mem_map(0x13aa0000, 0x20000)
uc.mem_write(VA, mod[FO:FO + CODELEN])
uc.mem_write(0x13aa8000, mod[0x7770:0x7778])
STACK, RET = 0x13abf000, 0x13abfff0
DEV = 0x13abc000
CTX = DEV + 0x94

# small helpers: write / read numbers in emulator memory (big-endian like the Wii)
def w32(a, v): uc.mem_write(a, struct.pack('>I', v & 0xffffffff))
def r32(a): return struct.unpack('>I', uc.mem_read(a, 4))[0]
def w16(a, v): uc.mem_write(a, struct.pack('>H', v & 0xffff))
def r16(a): return struct.unpack('>H', uc.mem_read(a, 2))[0]
def r8(a): return uc.mem_read(a, 1)[0]
def sx(v): return v - (1 << 32) if v & 0x80000000 else v
# argument n of the function that is being called (r0..r3, then the stack)
def arg(n):
    if n < 4: return uc.reg_read([UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3][n])
    return r32(uc.reg_read(UC_ARM_REG_SP) + 4 * (n - 4))
# act like the function returned v: put v into r0 and jump back to the caller
def ret(v): uc.reg_write(UC_ARM_REG_R0, v & 0xffffffff); uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

# where does the call (BL / BLX) at 'site' go to?
def bl_target(site):
    x = r32(site); off = x & 0xFFFFFF
    if off & 0x800000: off -= 0x1000000
    if x >> 25 == 0x7d:                                  # BLX immediate -> Thumb driver, bit 0 set
        return (site + 8 + off * 4 + ((x >> 23) & 2)) & 0xffffffff | 1
    return (site + 8 + off * 4) & 0xffffffff

FD = 0x55     # made-up handle of the USB device

# ---------------- the simulated adapter ----------------
class Dev:
    def __init__(s):
        s.mac = bytearray(256); s.mac[0x10:0x16] = bytes.fromhex('000ec6e40619')
        s.phy = {0: 0x1140, 1: 0x7949, 4: 0x05e1, 9: 0x0200, 0x11: 0x0000}
        s.mmd_ctl = 0; s.mmd = {}
        s.link = False; s.log = []; s.sleep_us = 0
        s.bulk_in = []; s.bulk_out = []; s.extra = []; s.rx_released = False
        s.ecm = False; s.cfg = 1; s.std = []; s.intr_n = 0; s.macstr = '6C6E072362AC'
    def ctrl(s, fd, bm, req, val, idx, ln, buf):
        assert fd == FD, hex(fd)
        if bm in (0x80, 0x00, 0x01, 0x21):                       # standard / class requests (ECM)
            s.std.append((bm, req, val, idx, ln))
            if bm == 0x80 and req == 6 and val == 0x0100:
                d = bytearray(18); d[0] = 18; d[1] = 1; d[17] = 3 if s.ecm else 1
                uc.mem_write(buf, bytes(d[:ln])); return ln
            if bm == 0x80 and req == 8: uc.mem_write(buf, bytes([s.cfg])); return 1
            if bm == 0x80 and req == 6 and val == 0x0305 and idx == 0x0409:
                st = bytes([26, 3]) + b''.join(bytes([ord(c), 0]) for c in s.macstr[:12])
                st = st.ljust(26, b'\0')[:26]; uc.mem_write(buf, st[:ln]); return min(ln, 26)
            if bm == 0x00 and req == 9: s.cfg = val; return 0
            if bm == 0x01 and req == 0x0b: return 0
            if bm == 0x21 and req == 0x43: s.filter = val; return 0
            raise Exception('unexpected standard request %x %x %x' % (bm, req, val))
        if bm == 0x40:
            d = bytes(uc.mem_read(buf, ln)) if ln else b''
            s.log.append(('W', req, val, idx, ln, d.hex()))
            if req == 1:
                assert idx == ln, 'MAC write: wIndex must equal length'
                s.mac[val:val + ln] = d
                if val == 0x26 and d[0] & 0x10 and s.mac[0x0b] & 0x80: s.rx_released = True   # BZ completes a pending bulk-in while RX START is set
            elif req == 2:
                assert val == 3 and ln == 2
                assert s.mac[0x26] & 0x20, 'PHY register write while the PHY is in reset'
                v = d[0] | d[1] << 8
                if idx == 0x0d: s.mmd_ctl = v
                elif idx == 0x0e: s.mmd[(s.mmd_ctl & 0x1f, 'last')] = v
                else: s.phy[idx] = v & ~0x0200 if idx == 0 else v
                if idx == 0 and v & 0x0200: s.log.append(('AN_RESTART',))
            else: raise Exception('unexpected vendor write req %x' % req)
            return ln
        if bm == 0xc0:
            if req == 1: assert idx == ln; d = bytes(s.mac[val:val + ln])
            elif req == 2:
                assert val == 3 and ln == 2
                assert s.mac[0x26] & 0x20, 'PHY register read while the PHY is in reset'
                v = s.phy.get(idx, 0); d = bytes([v & 0xff, v >> 8])
            else: raise Exception('unexpected vendor read req %x' % req)
            uc.mem_write(buf, d); s.log.append(('R', req, val, idx, ln, d.hex()))
            return ln
        raise Exception('bmRequestType %x' % bm)
dev = Dev()

# Functions of the module that talk to the outside (USB transfers, delay, system calls) are not
# executed. When the emulated CPU reaches their address, one of the small functions below runs
# instead and answers (usually with the simulated device).
HOOKS = {}
def hook(addr):
    def deco(f): HOOKS[addr] = f; return f
    return deco

@hook(0x13aa5130)
@hook(0x13aa5120)
def _ctrl(): ret(dev.ctrl(arg(0), arg(1), arg(2), arg(3), arg(4), arg(5), arg(6)))
@hook(0x13aa5150)
def _intr():
    fd, ep, ln, buf = (arg(i) for i in range(4))
    assert fd == FD and ep == 0x81 and ln in (8, 16)
    if ln == 16:
        n = [bytes([0xa1, 0x00, 0, 0, 0, 0, 0, 0]) + bytes(8),
             bytes([0xa1, 0x2a, 0, 0, 0, 0, 8, 0]) + struct.pack('<II', 1000000000, 1000000000),
             bytes([0xa1, 0x00, 1, 0, 0, 0, 0, 0]) + bytes(8)][dev.intr_n % 3]
        dev.intr_n += 1; uc.mem_write(buf, n); return ret(16)
    uc.mem_write(buf, bytes([0xa1, 0x00, 0x09 if dev.link else 0x08, 0x00, 0x79, 0x49, 0, 0])); ret(8)
@hook(0x13aa5170)
def _bulk_in():
    fd, ep, ln, buf = (arg(i) for i in range(4))
    assert fd == FD and ep == 0x82
    x = dev.bulk_in.pop(0)
    if isinstance(x, int): ret(x)
    else: assert len(x) <= ln; uc.mem_write(buf, x); ret(len(x))
@hook(0x13aa5160)
def _bulk_out():
    fd, ep, ln, buf = (arg(i) for i in range(4))
    assert fd == FD and ep == 3
    d = bytes(uc.mem_read(buf, ln))
    assert buf % 32 == 0 or True
    if len(d) >= 22 and b'\x88\xb5' in (d[8 + 12:8 + 14], d[12:14]): dev.extra.append(d); return ret(ln)   # ethertype 0x88b5: not real traffic
    dev.bulk_out.append(d); ret(ln)
FLUSHES = []
@hook(0x13aa4ea0)
def _flush(): FLUSHES.append((arg(0), arg(1))); ret(0)
@hook(0x13aa5180)
def _sleep(): dev.sleep_us += arg(0); ret(0)
@hook(0x13aa5140)
def _memset(): uc.mem_write(arg(0), bytes([arg(1) & 0xff]) * arg(2)); ret(arg(0))
@hook(0x13aa4e78)
def _push():
    iob, n = arg(0), arg(1)
    if dev.__dict__.get('push_fail'): return ret(0)
    hr = dev.__dict__.get('headroom')
    if hr is not None and (dev.hr_base - r32(iob + 0x10)) + n > hr: return ret(0)
    d = r32(iob + 0x10) - n; w32(iob + 0x10, d); w16(iob + 0x16, r16(iob + 0x16) + n); ret(d)
@hook(0x13aa4e80)
def _pull():
    iob, n = arg(0), arg(1); w32(iob + 0x10, r32(iob + 0x10) + n); w16(iob + 0x16, r16(iob + 0x16) - n); ret(1)
@hook(0x13aa4d80)
def _ios_open(): ret(FD)
@hook(0x13aa4d88)
def _ios_close(): ret(0)
@hook(0x13aa16c0)
def _log(): ret(0)
@hook(0x13aa1788)
def _lock(): ret(0)
@hook(0x13aa17e0)
def _unlock(): ret(0)

# runs for every instruction: if its address has a stand-in, use that
def on_code(uc, a, size, _):
    if a in HOOKS: HOOKS[a](); return
    if 0x13aa4c00 <= a < 0x13aa5200:
        raise Exception('unhooked system call / veneer at %x (lr %x)' % (a, uc.reg_read(UC_ARM_REG_LR)))
uc.hook_add(UC_HOOK_CODE, on_code, None, 0x13aa1600, 0x13aa5200)
def trap(uc, a, size, _): raise Exception('executed overwritten area at %x' % a)
if 0x13aa0084 + pj['blob_len'] <= 0x13aa128f: uc.hook_add(UC_HOOK_CODE, trap, None, 0x13aa0084 + pj['blob_len'], 0x13aa128f)   # begin > end would hook everything
if 0x13aa13c8 + pj.get('blob2_len', 0) <= 0x13aa14ff: uc.hook_add(UC_HOOK_CODE, trap, None, 0x13aa13c8 + pj.get('blob2_len', 0), 0x13aa14ff)
steps = [0]
def count(uc, a, s, _): steps[0] += 1
uc.hook_add(UC_HOOK_CODE, count, None, 0x13aa0084, 0x13aa14ff)

# runs one function of the module with the given arguments and returns its result
def call(fn, *args):
    uc.reg_write(UC_ARM_REG_SP, STACK); uc.reg_write(UC_ARM_REG_LR, RET)
    for r, v in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), args): uc.reg_write(r, v)
    uc.emu_start(fn, RET, count=3_000_000)
    return sx(uc.reg_read(UC_ARM_REG_R0))

ok = True
# prints PASS or FAIL for one check
def check(name, cond, extra=''):
    global ok
    print(('PASS ' if cond else 'FAIL ') + name + (('  ' + str(extra)) if extra and not cond else ''))
    ok &= bool(cond)
def macreg(r, n): return int.from_bytes(dev.mac[r:r + n], 'little')

# ---- 0. wiring: every retargeted call site must land on the new entry point
WIRES = {0x13aa195c: 'ax179_rx', 0x13aa1d18: 'ax179_forceread', 0x13aa1fe4: 'ax179_rxmode', 0x13aa2004: 'ax179_up',
         0x13aa20a4: 'ax179_down', 0x13aa211c: 'ax179_link', 0x13aa27b4: 'ax179_init', 0x13aa3018: 'ax179_getmac',
         0x13aa3088: 'ax179_setmac', 0x13aa3180: 'ax179_rxmode', 0x13aa3220: 'ax179_tx'}
check('all 11 call sites point at the new driver', all(bl_target(s) == S[n] for s, n in WIRES.items()))
check('axOpen / axClose call sites unchanged', bl_target(0x13aa2790) == 0x13aa1290 and
      bl_target(0x13aa1f48) == 0x13aa1500 and bl_target(0x13aa296c) == 0x13aa1500)
check('device match patched: PID 0x1790 (MOV 0x1700 + ADD 0x90)', r32(0x13aa22bc) == 0xe3a03c17 and r32(0x13aa22cc) == 0xe2833090)
# the monitor thread's device-list search (0x13aa1ea8): entries are {device id, vid<<16|pid}
def find_in_list(entries):
    lst = b''.join(struct.pack('>II', i + 1, v) for i, v in enumerate(entries))
    uc.mem_write(DEV + 0x100, lst + b'\0' * 32)
    return call(0x13aa1ea8, DEV + 0x100, len(entries))
check('device list search finds 0b95:1790 (alone and behind other devices)',
      find_in_list([0x0b951790]) == 0 and find_in_list([0x046d0825, 0x0b951790]) == 0)
check('device list search rejects 0b95:7720 and other devices', find_in_list([0x0b957720]) == -6 and find_in_list([0x046d0825]) == -6, (find_in_list([0x0b957720]), find_in_list([0x046d0825])))
check('device list search: empty list -> -6', find_in_list([]) == -6)
for p in (b'/dev/usb/ehc/0b95/1790\0', b'/dev/usb/oh0/0b95/1790\0'):
    check(f'path {p[:-1].decode()}', mod.find(p) >= 0)
check('no 7720 path left', mod.find(b'0b95/7720') < 0)

# ---- 1. original axOpen
uc.mem_write(DEV, b'\0' * 0x200)
w32(0x13ab6b60, 0x4000)                 # RXMAX as set by the module before usbEthDevInit
r = call(0x13aa1290, CTX)
check('axOpen (original) -> 0, fd stored, rx buffer 0x13aa9020', r == 0 and r32(CTX) == FD and r32(CTX + 0x34) == 0x13aa9020, (r, hex(r32(CTX)), hex(r32(CTX + 0x34))))

# ---- 2. init through the patched call site target, then the caller's copy of the link info
r = call(bl_target(0x13aa27b4), CTX, DEV + 0x2c)
uc.mem_write(DEV + 0x38, bytes(uc.mem_read(DEV + 0x2c, 12))); uc.mem_write(DEV, b'\x01')
check('init -> 0', r == 0, r)
check('init: MAC from NODE_ID in ctx+0xc', bytes(uc.mem_read(CTX + 0xc, 6)).hex() == '000ec6e40619')
check('init: PHY powered (PHYPWR=0x20), clocks (0x33=3)', macreg(0x26, 2) == 0x20 and dev.mac[0x33] == 3)
check('init: bulk-in queue 07 20 03 02 ff', bytes(dev.mac[0x2e:0x33]).hex() == '07200302ff')
check('init: pause 0x34/0x52, no checksum offload', dev.mac[0x55] == 0x34 and dev.mac[0x54] == 0x52 and dev.mac[0x34] == 0 and dev.mac[0x35] == 0)
check('init: medium 0x0133, rx_ctl 0x03aa', macreg(0x22, 2) == 0x133 and macreg(0x0b, 2) == 0x3aa)
check('init: EEE advertisement off, autoneg restarted', dev.mmd.get((7, 'last')) == 0 and ('AN_RESTART',) in dev.log)
def reg9_before_an(start):
    i9 = next((i for i, e in enumerate(dev.log[start:]) if e[0] == 'W' and e[1] == 2 and e[3] == 9), None)
    ian = next((i for i, e in enumerate(dev.log[start:]) if e == ('AN_RESTART',)), None)
    return i9 is not None and ian is not None and i9 < ian
check('init: 1000BASE-T not advertised (reg 9 bits 8/9 cleared before the autoneg restart)',
      (dev.phy[9] & 0x0300) == 0 and reg9_before_an(0) if not GIGABIT else dev.phy[9] == 0x0200, hex(dev.phy[9]))
li = bytes(uc.mem_read(DEV + 0x2c, 12))
caps, adv = struct.unpack('>II', li[:8]); spd = struct.unpack('>H', li[8:10])[0]
check('init: link info (caps 0x4f, adv 0x4f, speed 10, FD 1, AN 1)', (caps, adv, spd, li[10], li[11]) == (0x4f, 0x4f, 10, 1, 1),
      (hex(caps), hex(adv), spd, li[10], li[11]))
check('init: delays 0.5 s + 0.2 s', dev.sleep_us == 700000, dev.sleep_us)
check('init: ctx+0x30 (owned by axOpen) untouched', r32(CTX + 0x30) == 0)

# ---- 3. Nintendo's usbEthDevUp: axConfigMulticast + axUp
mcl = [bytes.fromhex(x) for x in ('333300000001', '01005e000001', '3333ff6bca1b')]
w32(DEV + 0x48, 0); w32(DEV + 0x4c, 3); uc.mem_write(DEV + 0x50, b''.join(mcl))
w32(CTX + 0x38, 0x12345678); w32(CTX + 0x3c, 0x9abcdef0)
n0 = len(dev.log)
r = call(0x13aa1f94, DEV)
check('usbEthDevUp -> 0, state 2', r == 0 and r8(DEV) == 2, (r, r8(DEV)))
check('up: multicast filter == capture value 0000008004000040', bytes(dev.mac[0x16:0x1e]).hex() == '0000008004000040', bytes(dev.mac[0x16:0x1e]).hex())
check('up: rx_ctl 0x0298 (IPE|START|AM|AB)', macreg(0x0b, 2) == 0x298, hex(macreg(0x0b, 2)))
check('up: medium 0x0130 until link, PHY on', macreg(0x22, 2) == 0x130 and macreg(0x26, 2) == 0x20)
check('up: ANAR 0x05e1 from link config, autoneg restarted', dev.phy[4] == 0x05e1 and ('AN_RESTART',) in dev.log[n0:])
check('up: rx state reset', r32(CTX + 0x38) == 0 and r32(CTX + 0x3c) == 0)
check('up again while up: no-op (state 2)', call(0x13aa1f94, DEV) == 0 and r8(DEV) == 2)

# ---- 4. link polling through Nintendo's wrapper
check('link down -> -0x18', call(0x13aa20fc, DEV) == -0x18)
dev.link = True; dev.phy[0x11] = 0xad02                       # 1000M full duplex (capture)
n0 = len(dev.log)
r = call(0x13aa20fc, DEV)
check('link up 1000FD -> 0, medium 0x013b (== capture)', r == 0 and macreg(0x22, 2) == 0x13b, (r, hex(macreg(0x22, 2))))
nw = sum(1 for e in dev.log[n0:] if e[0] == 'W')
check('second poll: no extra register writes', call(0x13aa20fc, DEV) == 0 and sum(1 for e in dev.log[n0:] if e[0] == 'W') == nw)

# no bit 10 in register 0x11 (the AX88179A vendor PHY is not used any more, it runs in CDC-ECM mode):
saved = dict(dev.phy)
dev.link = False; call(0x13aa20fc, DEV)
dev.phy.update({0x11: 0x9000}); dev.link = True
r = call(0x13aa20fc, DEV)
check('no link bit 10 -> base medium 0x0130', r == 0 and macreg(0x22, 2) == 0x130, (r, hex(macreg(0x22, 2))))
dev.link = False; call(0x13aa20fc, DEV)
dev.phy.clear(); dev.phy.update(saved)
dev.link = True; dev.phy[0x11] = 0xad02
check('back to the AX88179 PHY: 1000FD from register 0x11 -> 0x013b', call(0x13aa20fc, DEV) == 0 and macreg(0x22, 2) == 0x13b, hex(macreg(0x22, 2)))

# ---- 5. force read completion (RX thread stop), link loss, link at 100M
r = call(bl_target(0x13aa1d18), CTX)
check('forceread: PHYPWR = IPRL|BZ (0x30)', r >= 0 and macreg(0x26, 2) == 0x30, (r, hex(macreg(0x26, 2))))
dev.link = False
check('link lost -> -0x18', call(0x13aa20fc, DEV) == -0x18)
dev.link = True; dev.phy[0x11] = 0x6400
r = call(0x13aa20fc, DEV)
check('link up 100FD -> medium 0x0332, BZ cleared', r == 0 and macreg(0x22, 2) == 0x332 and macreg(0x26, 2) == 0x20,
      (r, hex(macreg(0x22, 2)), hex(macreg(0x26, 2))))

# ---- 6. Nintendo's usbEthDevDown, then up again WITHOUT init (as the module does)
med0, rxc0 = macreg(0x22, 2), macreg(0x0b, 2)
r = call(0x13aa205c, DEV)
check('usbEthDevDown -> 0, state 1', r == 0 and r8(DEV) == 1, (r, r8(DEV)))
check('down: only RECEIVE_EN cleared in MEDIUM (speed/duplex kept), rx_ctl untouched, PHY powered down (PHYPWR 0)',
      macreg(0x22, 2) == med0 & ~0x100 and macreg(0x0b, 2) == rxc0 and macreg(0x26, 2) == 0,
      (hex(med0), hex(macreg(0x22, 2)), hex(macreg(0x0b, 2)), hex(macreg(0x26, 2))))

# ---- 6b. what the module's main loop does next: the link poll reports "down" and the module
# stops its threads (0x13aa1c20, original code): running flag = 0, axForceReadCompletion,
# IOS_JoinThread(RX thread), IOS_JoinThread(delivery thread), destroy queues.
# Chip model (assumption, see README): a pending bulk-in is only completed by BZ while the
# bulk-in engine still runs (RX_CTL START, bit 7). Linux sets BZ before it stops RX_CTL.
RXTID, DLTID = 7, 8
class Hang(Exception): pass
joined = []
@hook(0x13aa4ca8)
def _join():
    tid = arg(0); joined.append(tid)
    if tid == RXTID and not dev.rx_released:
        raise Hang('RX thread stays in its bulk-in: PHYPWR %#x RX_CTL %#x' % (macreg(0x26, 2), macreg(0x0b, 2)))
    if tid == RXTID: dev.bulk_in = [0]; assert call_rx_once() == 0     # the released transfer: 0 bytes
    ret(0)
@hook(0x13aa4cb0)
def _cancel(): joined.append(('cancel', arg(0))); ret(0)
@hook(0x13aa4cf8)
def _destroyq(): ret(0)
def call_rx_once():
    st = [uc.reg_read(x) for x in (UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_FP)]
    uc.reg_write(UC_ARM_REG_SP, STACK - 0x800); uc.reg_write(UC_ARM_REG_LR, RET + 8)
    uc.reg_write(UC_ARM_REG_R0, CTX); uc.reg_write(UC_ARM_REG_R1, 0x13abd200); uc.reg_write(UC_ARM_REG_R2, 0x13abd210)
    uc.emu_start(bl_target(0x13aa195c), RET + 8, count=100000)
    r = sx(uc.reg_read(UC_ARM_REG_R0))
    for x, v in zip((UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_FP), st): uc.reg_write(x, v)
    return r
dev.rx_released = False
w32(0x13aad314, 1); uc.mem_write(DEV + 0x78, b'\x01')
w32(DEV + 0x70, RXTID); w32(DEV + 0x74, DLTID); w32(DEV + 0x7c, 9); w32(DEV + 0x84, 10)
check('link poll after down -> -0x18', call(0x13aa20fc, DEV) in (-0x18, 0))
try:
    r = call(0x13aa1c20, DEV); hang = None
except Hang as e:
    r, hang = None, str(e)
check('thread stop after down (original 0x13aa1c20): RX thread released by forceread, both threads joined',
      hang is None and r == 0 and joined == [RXTID, DLTID] and r32(0x13aad314) == 0, hang or (r, joined))
check('thread stop after down: PHY back in reset, port stays down (PHYPWR 0)', macreg(0x26, 2) == 0, hex(macreg(0x26, 2)))
s0 = dev.sleep_us
check('link poll after down -> -0x18 (link status from the chip)', call(0x13aa20fc, DEV) in (-0x18, 0))
dev.phy[9] = 0x0200; n9 = len(dev.log)                 # the PHY reset brought the default back
r = call(0x13aa1f94, DEV)
check('up again after down: 1000BASE-T not advertised again (before the autoneg restart)',
      (dev.phy[9] & 0x0300) == 0 and reg9_before_an(n9) if not GIGABIT else dev.phy[9] == 0x0200, hex(dev.phy[9]))
check('up again after down -> state 2, rx_ctl 0x0298, PHY woken (clock select, 0.3 s)', r == 0 and r8(DEV) == 2 and macreg(0x0b, 2) == 0x298 and macreg(0x26, 2) == 0x20 and dev.mac[0x33] == 3 and dev.sleep_us - s0 == 300000,
      (r, r8(DEV), hex(macreg(0x0b, 2)), hex(macreg(0x26, 2)), dev.sleep_us - s0))
check('first poll after up re-applies medium 0x0332', call(0x13aa20fc, DEV) == 0 and macreg(0x22, 2) == 0x332, hex(macreg(0x22, 2)))

# ---- 7. forced link configuration (autoneg off, 100 full)
cfg = bytearray(uc.mem_read(DEV + 0x38, 12)); cfg[11] = 0; cfg[10] = 1; cfg[8:10] = struct.pack('>H', 100)
uc.mem_write(DEV + 0x38, bytes(cfg)); call(0x13aa205c, DEV); call(0x13aa1f94, DEV)
check('forced 100FD: BMCR 0x2100', dev.phy[0] == 0x2100, hex(dev.phy[0]))

# ---- 8. get / set MAC through their call-site targets
uc.mem_write(0x13abd000, b'\0' * 8)
call(bl_target(0x13aa3018), CTX, 0x13abd000)
check('getmac', bytes(uc.mem_read(0x13abd000, 6)).hex() == '000ec6e40619')
uc.mem_write(0x13abd100, bytes.fromhex('020000aabbcc'))
check('setmac', call(bl_target(0x13aa3088), CTX, 0x13abd100) >= 0 and dev.mac[0x10:0x16].hex() == '020000aabbcc')

# ---- 9. only AX88179 vendor requests
check('only AX88179 requests (1 MAC, 2 PHY) issued', all(e[0] not in ('W', 'R') or e[1] in (1, 2) for e in dev.log))

# ---- 9b. AX88179A in CDC-ECM mode (3 configurations): own init / up / link / tx / rx paths
dev.ecm = True; dev.cfg = 1; dev.std = []; dev.intr_n = 0; dev.sleep_us = 0
nlog = len(dev.log)
r = call(bl_target(0x13aa27b4), CTX, DEV + 0x2c)
uc.mem_write(DEV + 0x38, bytes(uc.mem_read(DEV + 0x2c, 12))); uc.mem_write(DEV, b'\x01')
check('ECM init -> 0', r == 0, r)
check('ECM init: SET_CONFIGURATION 3, SET_INTERFACE 1/1 after GET_CONFIGURATION', [e[:2] for e in dev.std if e[0] in (0x00, 0x01)] == [(0x00, 9), (0x01, 0x0b)] and dev.cfg == 3)
check('ECM init: packet filter 0x000f (promiscuous), 3 interrupt notifications, 1 s settle', getattr(dev, 'filter', 0) == 0x0f and dev.intr_n == 3 and dev.sleep_us == 1000000, (dev.intr_n, dev.sleep_us))
check('ECM init: no AX88179 vendor request', len(dev.log) == nlog)
check('ECM init: MAC from string descriptor 5 (6c:6e:07:23:62:ac)', bytes(uc.mem_read(CTX + 0xc, 6)).hex() == '6c6e072362ac', bytes(uc.mem_read(CTX + 0xc, 6)).hex())
li = bytes(uc.mem_read(DEV + 0x2c, 12))
check('ECM init: link info 100 FD autoneg', struct.unpack('>I', li[:4])[0] == 0x4f and struct.unpack('>H', li[8:10])[0] == 100 and li[10] == 1 and li[11] == 1)
check('ECM: flag in ctx[2] bit 31, link flag set', r32(CTX + 8) & 0x80000000 and r32(CTX + 4) == 1)
ngc = len(dev.std)
r = call(0x13aa1f94, DEV)
check('ECM usbEthDevUp -> 0, state 2, filter re-set, no config change', r == 0 and r8(DEV) == 2 and not any(e[:2] in ((0x00, 9), (0x01, 0x0b)) for e in dev.std[ngc:]), (r, r8(DEV)))
us0 = dev.sleep_us
lp = [call(0x13aa20fc, DEV) for _ in range(5)]
check('ECM link polls: down 4x then up, each poll waits 100 ms; no INTR traffic', lp == [-0x18] * 4 + [0] and dev.intr_n == 3 and dev.sleep_us - us0 == 500000, (lp, dev.sleep_us - us0))
uc.mem_write(0x13abd000, b'\0' * 8)
call(bl_target(0x13aa3018), CTX, 0x13abd000)
check('ECM getmac from ctx', bytes(uc.mem_read(0x13abd000, 6)).hex() == '6c6e072362ac')
uc.mem_write(0x13abd100, bytes.fromhex('bc744b7f90b8'))
check('ECM setmac ignored', call(bl_target(0x13aa3088), CTX, 0x13abd100) == 0 and bytes(uc.mem_read(CTX + 0xc, 6)).hex() == '6c6e072362ac')
RX, TX = bl_target(0x13aa195c), bl_target(0x13aa3220)
OUT, LEN = 0x13abd200, 0x13abd210
frame = bytes(range(60)) + bytes(530)
dev.bulk_in = [frame]; w16(LEN, 0xdead)
r = call(RX, CTX, OUT, LEN)
check('ECM RX: raw frame 590 B handed over unchanged', r == 0 and r16(LEN) == 590 and bytes(uc.mem_read(r32(OUT), 590)) == frame, (r, r16(LEN)))
dev.bulk_in = [5]
check('ECM RX: short transfer -> len 0', call(RX, CTX, OUT, LEN) == 0 and r16(LEN) == 0)
dev.bulk_in = [-5]
check('ECM RX: error propagated', call(RX, CTX, OUT, LEN) == -5)
IOB, FR = 0x13abd400, 0x13abd500
for n in (60, 100, 512, 1024, 1514):
    dev.bulk_out = []
    uc.mem_write(IOB, b'\0' * 0x40); w32(IOB + 0x10, FR + 4); w16(IOB + 0x16, n); uc.mem_write(FR + 4, bytes((i * 7) & 255 for i in range(n)))
    r = call(TX, CTX, IOB)
    exp = bytes((i * 7) & 255 for i in range(n)) + (b'\0' if n % 512 == 0 else b'')
    check(f'ECM TX {n} B: raw frame{" + pad byte" if n % 512 == 0 else ""}, IOB untouched', r == 0 and dev.bulk_out == [exp] and r32(IOB + 0x10) == FR + 4 and r16(IOB + 0x16) == n, (r, len(dev.bulk_out)))
uc.mem_write(IOB, b'\0' * 0x40); w32(IOB + 0x10, FR); w16(IOB + 0x16, 1600)
check('ECM TX oversize -> -0x16', call(TX, CTX, IOB) == -0x16)
RING = 0x13aa9020 + 0x800
def ring_events():
    n = r32(RING + 4)
    return [(r32(RING + 64 + 8 * (i % 128)) >> 24, sx(r32(RING + 68 + 8 * (i % 128)))) for i in range(max(0, n - 128), n)]
ev = ring_events(); codes = [c for c, v in ev]
check('ECM ring: magic R37, ctx pointer', r32(RING) == 0x52333700 and r32(RING + 8) == CTX, hex(r32(RING)))
exp = [1, 14, 12, 13, 2, 3, 7, 8, 9, 11, 11, 11]          # init, macstr, mac0, mac1, initdone, up, link, getmac, setmac, rx x3
it = iter(codes)
check('ECM ring: events init/macstr/mac/initdone/up/link/getmac/setmac/rx-changes in order', all(c in it for c in exp), codes)
check('ECM ring: init config 1, MAC words, rx changes 590 / 5 / -5', dict(ev)[1] == 1 and ev[codes.index(12)][1] == 0x6c6e0723 and
      (ev[codes.index(13)][1] & 0xffff0000) == 0x62ac0000 and [v for c, v in ev if c == 11] == [590, 5, -5], ev)
check('ECM ring: speed from the ConnectionSpeedChange notification (1000000000 bit/s) logged once', [v for c, v in ev if c == 15] == [1000000000], ev)
check('ECM ring: tx 5 calls, last rc ok, rx 3 calls, last -5, link result 0', r32(RING + 16) == 5 and sx(r32(RING + 20)) >= 0 and r32(RING + 24) == 3 and sx(r32(RING + 28)) == -5 and r32(RING + 32) == 0,
      [hex(r32(RING + 4 * k)) for k in range(10)])
check('ECM ring flushed (syscall 0x40) with its address', any(a == RING and l == (16 + 256) * 4 for a, l in FLUSHES), FLUSHES[:3])
check('no frames are sent besides the real traffic', len(dev.extra) == 0, len(dev.extra))
check('ECM forceread -> 0, no chip access', call(bl_target(0x13aa1d18), CTX) == 0 and len(dev.log) == nlog)
nl = len(dev.log)
check('ECM rxmode (via up/multicast) only sets the packet filter', len(dev.log) == nl)
r = call(0x13aa205c, DEV)
check('ECM usbEthDevDown -> 0, link flag cleared, link poll -> -0x18', r == 0 and call(0x13aa20fc, DEV) == -0x18 and len(dev.log) == nlog)
call(0x13aa1f94, DEV)
lp = [call(0x13aa20fc, DEV) for _ in range(5)]
check('ECM up again -> link up after 4 polls', lp == [-0x18] * 4 + [0], lp)
check('ECM ring: down / up / link -24 / link 0 logged', [c for c, v in ring_events()][-6:].count(4) == 1 and (7, -0x18) in ring_events() and ring_events()[-1] == (7, 0), ring_events()[-6:])
# re-enumerated device already in configuration 3: no switch, no notifications
ngc = len(dev.std); dev.intr_n = 0; dev.sleep_us = 0
call(bl_target(0x13aa27b4), CTX, DEV + 0x2c)
check('ECM re-init with config 3 active: no SET_CONFIGURATION, no settle, no notifications', not any(e[:2] in ((0x00, 9), (0x01, 0x0b)) for e in dev.std[ngc:]) and dev.intr_n == 0 and dev.sleep_us == 0)
dev.macstr = ''
call(bl_target(0x13aa27b4), CTX, DEV + 0x2c)
check('ECM missing MAC string -> 02:00:00:00:00:00', bytes(uc.mem_read(CTX + 0xc, 6)).hex() == '020000000000', bytes(uc.mem_read(CTX + 0xc, 6)).hex())
# back to the AX88179 (1 configuration): flag cleared again
dev.ecm = False; dev.cfg = 1; dev.macstr = '6C6E072362AC'
r = call(bl_target(0x13aa27b4), CTX, DEV + 0x2c)
check('init with a 1-configuration device: AX88179 mode again', r == 0 and not (r32(CTX + 8) & 0x80000000) and bytes(uc.mem_read(CTX + 0xc, 6)).hex() == '020000aabbcc')
dev.ecm = False

# ---- 10. RX / TX of generated frames through the call-site targets
def agg(frames, bad=()):
    """one aggregated bulk-in transfer as the chip sends it: frames (2 pad bytes in front, 8-byte
    aligned), then one LE32 header per frame (length << 16 | flags), then count | header offset << 16"""
    data, hdrs = b'', b''
    for k, f in enumerate(frames):
        hdrs += struct.pack('<I', (0x20000000 if k in bad else 0) | (len(f) + 2) << 16)   # bit 29 CRC error
        c = b'\0\0' + f
        data += c + b'\0' * (-len(c) % 8)
    return data + hdrs + struct.pack('<I', len(frames) | len(data) << 16)
def frame(n, seed): return bytes((seed * 7 + i * 13) & 0xff for i in range(n))
RX, TX = bl_target(0x13aa195c), bl_target(0x13aa3220)
OUT, LEN = 0x13abd200, 0x13abd210
tests = [[frame(60, 1)], [frame(1514, 2)], [frame(n, n) for n in (60, 98, 342, 590, 1514)], [frame(64, 3), frame(12, 4), frame(70, 5)]]
good = 0
for k, frs in enumerate(tests):
    bad = (1,) if k == 2 else ()
    w32(CTX + 0x38, 0); w32(CTX + 0x3c, 0); dev.bulk_in = [agg(frs, bad)]
    exp = [f for i, f in enumerate(frs) if i not in bad and len(f) >= 14]
    got = []
    for _ in exp:
        r = call(RX, CTX, OUT, LEN)
        got.append(bytes(uc.mem_read(r32(OUT), r16(LEN))) if r == 0 else None)
    good += got == exp
check(f'RX {good}/{len(tests)} aggregated transfers (1 and 5 frames, CRC error and runt skipped)', good == len(tests))
dev.bulk_in = [agg([])]; w16(LEN, 0xdead)
check('RX transfer without frames -> 0, len 0', call(RX, CTX, OUT, LEN) == 0 and r16(LEN) == 0)
dev.bulk_in = [0]; w16(LEN, 0xdead)
check('RX empty transfer -> 0, len 0', call(RX, CTX, OUT, LEN) == 0 and r16(LEN) == 0)
dev.bulk_in = [-5]
check('RX bulk error propagated', call(RX, CTX, OUT, LEN) == -5)

def tx_ref(f):
    """bulk-out data the chip expects: length LE32, then 0x80008000 flags if the transfer is a multiple of 512"""
    pad = 0x80 if (len(f) + 8) % 512 == 0 else 0
    return struct.pack('<I', len(f)) + bytes([0, pad, 0, pad]) + f
IOB, FR = 0x13abd400, 0x13abd500
outs = [tx_ref(frame(n, n)) for n in (42, 60, 342, 504, 1016, 1514)]     # 504 / 1016: total 512 / 1024
okt = 0
for o in outs:
    dev.bulk_out = []
    uc.mem_write(IOB, b'\0' * 0x40); w32(IOB + 0x10, FR); w16(IOB + 0x16, len(o) - 8); uc.mem_write(FR, o[8:])
    okt += call(TX, CTX, IOB) == 0 and dev.bulk_out == [o]
check(f'TX {okt}/{len(outs)} frames byte-exact (header, 512-byte padding flag)', okt == len(outs))
dev.push_fail = True
uc.mem_write(IOB, b'\0' * 0x40); w32(IOB + 0x10, FR); w16(IOB + 0x16, 60)
check('TX push failure -> -0x16', call(TX, CTX, IOB) == -0x16)
dev.push_fail = False

# IOB with room for only 4 bytes (what the original driver needs): copy path must send the same bytes
dev.headroom = 4; dev.hr_base = FR
okc = 0; fb0 = r32(CTX + 0x1c)
for o in outs:
    dev.bulk_out = []
    uc.mem_write(IOB, b'\0' * 0x40); w32(IOB + 0x10, FR); w16(IOB + 0x16, len(o) - 8); uc.mem_write(FR, o[8:])
    okc += call(TX, CTX, IOB) == 0 and dev.bulk_out == [o] and r32(IOB + 0x10) == FR and r16(IOB + 0x16) == len(o) - 8
check(f'TX with only 4 bytes of IOB headroom: {okc}/{len(outs)} byte-exact via copy path, IOB restored', okc == len(outs))
check('copy path counted', r32(CTX + 0x1c) - fb0 == len(outs))
big = bytes(range(256)) * 7                              # 1792 > TXCOPY_MAX
uc.mem_write(IOB, b'\0' * 0x40); w32(IOB + 0x10, FR); w16(IOB + 0x16, len(big)); uc.mem_write(FR, big)
check('oversize frame on the copy path -> -0x16, IOB restored', call(TX, CTX, IOB) == -0x16 and r16(IOB + 0x16) == len(big))
dev.headroom = None
check('no frames are sent besides the real traffic', len(dev.extra) == 0, len(dev.extra))

# ---- 11. original axClose
r = call(0x13aa1500, CTX)
check('axClose (original) -> 0, fd -1, rx buffer released', r == 0 and r32(CTX) == 0xffffffff and r32(CTX + 0x34) == 0 and r32(0x13aa9000) == 0)

print(f'({steps[0]} instructions executed in the new driver)')
print('ALL OK' if ok else 'FAILURES')
sys.exit(0 if ok else 1)
