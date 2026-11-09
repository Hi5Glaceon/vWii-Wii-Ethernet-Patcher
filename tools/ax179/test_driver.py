#!/usr/bin/env python3
"""Emulator test for the patched IOS58 / IOS57 / IOS59 ETH module (the Thumb modules).

What does it do?
  It loads the PATCHED module (eth<ios>_patched.app) into Unicorn, a CPU emulator, and runs the
  module's own code. Instead of a real USB adapter there is a small simulated AX88179 in this
  file (class Dev). It answers the USB requests of the driver like the real chip would.

  The test calls the same functions the IOS calls, in the same order: open the device, bring the
  interface up, poll the link (link down, up at 1000M / 100M / 10M), get / set the MAC, stop the
  receive thread, take the interface down, send and receive frames. After each step it checks
  that the chip registers and the data in the module are what they should be.

How to read the output:
  PASS <text>    a check worked
  FAIL <text>    a check failed (the values are printed behind it)
  ALL OK         at the end, if nothing failed

Usage:  test_driver.py <58|57|59>      (run in tools/ax179, after build_patch.py for the same IOS)
GIGABIT=1 in the environment if the driver was built with GIGABIT=1.
"""
import json, os, struct, sys
GIGABIT = os.environ.get('GIGABIT', '0') == '1'   # build with 1000BASE-T advertised
from unicorn import *
from unicorn.arm_const import *

VA, FO = 0x13aa0000, 0x108
IOS = int(sys.argv[1]) if len(sys.argv) > 1 else 58
mod = open(f'eth{IOS}_patched.app', 'rb').read()
data = json.load(open(f'patch_data_{IOS}.json'))
S = data['syms']
V = data['iob_veneer'] - 0x13aa2a68             # IOS59: syscall veneers 16 bytes further up

uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_BIG_ENDIAN)
uc.mem_map(0x13aa0000, 0x30000)
uc.mem_write(VA, mod[FO:])
STACK, RET = 0x13abf000, 0x13abfff0

# small helpers: write / read numbers in emulator memory (big-endian like the Wii)
def w32(a, v): uc.mem_write(a, struct.pack('>I', v & 0xffffffff))
def r32(a): return struct.unpack('>I', uc.mem_read(a, 4))[0]
def w16(a, v): uc.mem_write(a, struct.pack('>H', v & 0xffff))
def r16(a): return struct.unpack('>H', uc.mem_read(a, 2))[0]
def r8(a): return uc.mem_read(a, 1)[0]
def sx(v): return v - (1 << 32) if v & 0x80000000 else v
# argument n of the function that is being called (r0..r3, then the stack)
def arg(n): return uc.reg_read([UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3][n]) if n < 4 \
    else r32(uc.reg_read(UC_ARM_REG_SP) + 4 * (n - 4))
# act like the function returned v: put v into r0 and jump back to the caller
def ret(v): uc.reg_write(UC_ARM_REG_R0, v & 0xffffffff); uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

FD, EP0H = 0x55, 0x66     # made-up handles of the USB device and of its endpoint 0
RXTID = 7                 # made-up id of the receive thread
class Hang(Exception): pass

# ---------------- simulated AX88179 ----------------
class Dev:
    def __init__(s):
        s.mac = bytearray(256); s.mac[0x10:0x16] = bytes.fromhex('000ec6e40619')
        s.phy = {0: 0x1140, 1: 0x7949, 4: 0x05e1, 9: 0x0200, 0x11: 0x0000}
        s.mmd_ctl = 0; s.mmd = {}
        s.link = False; s.log = []; s.delay_us = 0
        s.bulk_in = []; s.bulk_out = []; s.rx_released = False
    def ctrl(s, fd, ep0, bm, req, val, idx, ln, buf):
        assert fd == FD and ep0 == EP0H, (hex(fd), hex(ep0))
        if bm == 0x80 and req == 6 and val == 0x0100:          # device descriptor: 1 configuration
            d = bytes.fromhex('1201100200000040950b9017000101020301')[:ln]
            uc.mem_write(buf, d); s.log.append(('S', req, val)); return ln
        if bm == 0x80 and req == 8:                           # GET_CONFIGURATION
            uc.mem_write(buf, b'\x01'); s.log.append(('S', req, val)); return 1
        if bm == 0x40:
            d = bytes(uc.mem_read(buf, ln)) if ln else b''
            s.log.append(('W', req, val, idx, ln, d.hex()))
            if req == 1:
                assert idx == ln, 'MAC write: wIndex must equal length'
                s.mac[val:val + ln] = d
                if val == 0x26 and d[0] & 0x10 and s.mac[0x0b] & 0x80: s.rx_released = True   # model: BZ completes a pending bulk-in only while RX START is set
            elif req == 2:
                assert val == 3 and ln == 2
                v = d[0] | d[1] << 8
                if idx == 0x0d: s.mmd_ctl = v
                elif idx == 0x0e: s.mmd[(s.mmd_ctl & 0x1f, 'last')] = v
                else: s.phy[idx] = v & ~0x0200 if idx == 0 else v   # restart bit self-clears
                if idx == 0 and v & 0x0200: s.log.append(('AN_RESTART',))
            else: raise Exception('unexpected vendor write req %x' % req)
            return ln
        if bm == 0xc0:
            if req == 1: d = bytes(s.mac[val:val + ln]); assert idx == ln
            elif req == 2:
                assert val == 3 and ln == 2
                v = s.phy.get(idx, 0); d = bytes([v & 0xff, v >> 8])
            else: raise Exception('unexpected vendor read req %x' % req)
            uc.mem_write(buf, d); s.log.append(('R', req, val, idx, ln, d.hex()))
            return ln
        raise Exception('bmRequestType %x' % bm)
    def intr(s, buf, ln, ep):
        assert ep == 0x81 and ln == 8
        uc.mem_write(buf, bytes([0xa1, 0x00, 0x09 if s.link else 0x08, 0x00, 0x79, 0x49, 0, 0]))
        return 8
dev = Dev()

# This runs whenever the emulated CPU reaches the start of a function of the module that talks to
# the outside (USB transfers, delay, system calls). The function is not executed; the simulated
# device (or a stub) answers instead.
def hook(uc, a, size, _):
    if a == 0x13aa1ac0:
        ret(dev.ctrl(arg(0), arg(1), arg(2), arg(3), arg(4), arg(5), arg(6), arg(7)))
    elif a == 0x13aa1b24:
        assert arg(0) == FD and arg(1) == EP0H
        ret(dev.intr(arg(2), arg(3), arg(4)))
    elif a == 0x13aa1b7c:
        fd, ep0, buf, ln, ep = (arg(i) for i in range(5))
        assert fd == FD and ep0 == EP0H
        if ep == 0x82:
            dev.last_in_len = ln
            x = dev.bulk_in.pop(0)
            if isinstance(x, int): ret(x)
            else: assert len(x) <= ln; uc.mem_write(buf, x); ret(len(x))
        else:
            dev.bulk_out.append(bytes(uc.mem_read(buf, ln))); ret(ln)
    elif a == 0x13aa23f4:
        dev.delay_us += arg(0); ret(0)
    elif a == 0x13aa2a68 + V:                  # IOB push
        iob, n = arg(0), arg(1); d = r32(iob + 0x10) - n
        w32(iob + 0x10, d); w16(iob + 0x16, r16(iob + 0x16) + n); ret(d)
    elif a == 0x13aa2a70 + V:                  # IOB pull
        iob, n = arg(0), arg(1); w32(iob + 0x10, r32(iob + 0x10) + n); w16(iob + 0x16, r16(iob + 0x16) - n); ret(1)
    elif a == 0x13aa29e8 + V:                  # memcpy veneer (dst, src, n)
        uc.mem_write(arg(0), bytes(uc.mem_read(arg(1), arg(2)))); ret(arg(0))
    elif a == 0x13aa2a08 + V and arg(0) == RXTID:   # IOS_JoinThread(RX thread)
        if not dev.rx_released: raise Hang('RX thread stays in its bulk-in: PHYPWR %#x RX_CTL %#x' % (macreg(0x26, 2), macreg(0x0b, 2)))
        ret(0)
    elif 0x13aa29b4 + V <= a < 0x13aa2a90 + V: # any other IOS syscall veneer: succeed
        ret(0)
uc.hook_add(UC_HOOK_CODE, hook, None, 0x13aa1ac0, 0x13aa1ac0)
uc.hook_add(UC_HOOK_CODE, hook, None, 0x13aa1b24, 0x13aa1b24)
uc.hook_add(UC_HOOK_CODE, hook, None, 0x13aa1b7c, 0x13aa1b7c)
uc.hook_add(UC_HOOK_CODE, hook, None, 0x13aa23f4, 0x13aa23f4)
uc.hook_add(UC_HOOK_CODE, hook, None, 0x13aa29b4 + V, 0x13aa2a90 + V)
def trap(uc, a, size, _): raise Exception('executed unused block area at %x' % a)
uc.hook_add(UC_HOOK_CODE, trap, None, 0x13aa0da4 + data['blob_len'], 0x13aa180f)
uc.hook_add(UC_HOOK_CODE, trap, None, 0x13aa18cc + data['blob2_len'], 0x13aa195f)
cancels = []
def cancel_hook(uc, a, size, _):                # FUN_13aa1bf8(fd, ep0, ep): USBV5 CANCELENDPOINT
    assert arg(0) == FD and arg(1) == EP0H
    cancels.append(arg(2))
    if arg(2) == 0x82: dev.rx_released = True
    ret(-4)                                     # libogc2 saw -4 from CANCELENDPOINT on the Wii U
uc.hook_add(UC_HOOK_CODE, cancel_hook, None, 0x13aa1bf8, 0x13aa1bf8)

# runs one function of the module with the given arguments and returns its result
def call(fn, *args):
    uc.reg_write(UC_ARM_REG_SP, STACK); uc.reg_write(UC_ARM_REG_LR, RET | 1)
    for r, v in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), args): uc.reg_write(r, v)
    uc.emu_start(fn | 1, RET, count=2_000_000)
    return sx(uc.reg_read(UC_ARM_REG_R0))

ok = True
# prints PASS or FAIL for one check
def check(name, cond, extra=''):
    global ok
    print(('PASS ' if cond else 'FAIL ') + name + (('  ' + str(extra)) if extra and not cond else ''))
    ok &= bool(cond)
def macreg(r, n): return int.from_bytes(dev.mac[r:r + n], 'little')

P = 0x13ab4000; CTX = P + 0x9c
uc.mem_write(P, b'\0' * 0x200)
w32(P + 8, FD); w32(P + 0xc, EP0H)
w32(0x13ab0140, 0x1000)                 # set by FUN_13aa04e8 after FUN_13aa19b0 returns 1
for o in (0x74, 0x78, 0x84, 0x98): w32(P + o, 0xffffffff)
# multicast list used on "up": flags=0, count=3
mcl = [bytes.fromhex(x) for x in ('333300000001', '01005e000001', '3333ff6bca1b')]
w32(P + 0x50, 0); w32(P + 0x54, 3); uc.mem_write(P + 0x58, b''.join(mcl))

# ---- 1. open (FUN_13aa0454) ----
call(0x13aa0454, P)
check('open: device state = 1', r8(P + 1) == 1, r8(P + 1))
check('open: fd/ep0 stored', r32(0x13ab0184) == FD and r32(0x13ab0188) == EP0H)
check('open: MAC from NODE_ID in ctx+0xc', bytes(uc.mem_read(CTX + 0xc, 6)).hex() == '000ec6e40619')
check('open: PHY powered (PHYPWR=0x20), clocks (0x33=3)', macreg(0x26, 2) == 0x20 and dev.mac[0x33] == 3)
check('open: bulk-in queue 07 20 03 02 ff', bytes(dev.mac[0x2e:0x33]).hex() == '07200302ff')
check('open: pause 0x34/0x52, no COE', dev.mac[0x55] == 0x34 and dev.mac[0x54] == 0x52 and dev.mac[0x34] == 0 and dev.mac[0x35] == 0)
check('open: medium 0x0133, rx_ctl 0x03aa', macreg(0x22, 2) == 0x133 and macreg(0x0b, 2) == 0x3aa)
check('open: autoneg restarted', ('AN_RESTART',) in dev.log)
def reg9_before_an(start):
    i9 = next((i for i, e in enumerate(dev.log[start:]) if e[0] == 'W' and e[1] == 2 and e[3] == 9), None)
    ian = next((i for i, e in enumerate(dev.log[start:]) if e == ('AN_RESTART',)), None)
    return i9 is not None and ian is not None and i9 < ian
check('open: 1000BASE-T not advertised (reg 9 bits 8/9 cleared before the autoneg restart)',
      (dev.phy[9] & 0x0300) == 0 and reg9_before_an(0) if not GIGABIT else dev.phy[9] == 0x0200, hex(dev.phy[9]))
check('open: EEE adv disabled (MMD7 0x3c := 0)', dev.mmd.get((7, 'last')) == 0)
li = bytes(uc.mem_read(P + 0x34, 12))
caps, adv = struct.unpack('>II', li[:8]); spd = struct.unpack('>H', li[8:10])[0]
check('open: link info (caps 0x4f, adv 0x4f, speed 10, FD 1, AN 1)',
      (caps, adv, spd, li[10], li[11]) == (0x4f, 0x4f, 10, 1, 1), (hex(caps), hex(adv), spd, li[10], li[11]))
check('open: link info copied to +0x40', bytes(uc.mem_read(P + 0x40, 12)) == li)
check('open: delays 0.5 s + 0.2 s', dev.delay_us == 700000, dev.delay_us)

# ---- 2. interface up (FUN_13aa0358): init + rx mode + rx reset ----
w32(CTX + 0x38, 0x12345678); w32(CTX + 0x3c, 0x9abcdef0)
call(0x13aa0358, P)
check('up: device state = 2', r8(P + 1) == 2, r8(P + 1))
check('up: multicast filter == capture value 0000008004000040', bytes(dev.mac[0x16:0x1e]).hex() == '0000008004000040',
      bytes(dev.mac[0x16:0x1e]).hex())
check('up: rx_ctl 0x0298 (IPE|START|AM|AB)', macreg(0x0b, 2) == 0x298, hex(macreg(0x0b, 2)))
check('up: rx state reset', r32(CTX + 0x38) == 0 and r32(CTX + 0x3c) == 0)

# promisc / allmulti flags via rx mode directly
w32(0x13ab3000, 0x18); w32(0x13ab3004, 0)
call(S['ax179_rxmode'], CTX, 0x13ab3000)
check('rxmode: promisc|allmulti -> 0x029b, empty filter', macreg(0x0b, 2) == 0x29b and bytes(dev.mac[0x16:0x1e]) == bytes(8))
call(0x13aa0358, P)   # state 2: no-op path in original code, must not re-init

# ---- 3. link polling (FUN_13aa0418) ----
n0 = len(dev.log)
r = call(0x13aa0418, P)
check('link down -> -0x18', r == -0x18, r)
dev.link = True; dev.phy[0x11] = 0xad02             # 1000M full duplex (from capture)
r = call(0x13aa0418, P)
check('link up 1000FD -> 0, medium 0x013b (== capture)', r == 0 and macreg(0x22, 2) == 0x13b, (r, hex(macreg(0x22, 2))))
nw = sum(1 for e in dev.log[n0:] if e[0] == 'W')
r = call(0x13aa0418, P)
check('second poll: no extra register writes', r == 0 and sum(1 for e in dev.log[n0:] if e[0] == 'W') == nw)

# AX88179A PHY (other layout of register 0x11: 0x9000 at 1000M full, from a usbmon trace);
# speed / duplex must then come from the standard registers 9/10 and 4/5
saved = dict(dev.phy)
dev.link = False; call(0x13aa0418, P)
dev.phy.update({0x11: 0x9000, 9: 0x0200, 10: 0x7800, 5: 0xc5e1}); dev.link = True
r = call(0x13aa0418, P)
check('no link bit in register 0x11 (AX88179A vendor mode): speed bits stay at the base value', r == 0 and macreg(0x22, 2) == 0x130, (r, hex(macreg(0x22, 2))))
dev.link = False; call(0x13aa0418, P)
dev.phy.update({0x11: 0x0000, 10: 0x0000, 5: 0x45e1}); dev.link = True
r = call(0x13aa0418, P)
dev.link = False; call(0x13aa0418, P)
dev.phy.update({0x11: 0x6000, 5: 0x0021}); dev.link = True
r = call(0x13aa0418, P)
dev.link = False; call(0x13aa0418, P)
dev.phy.clear(); dev.phy.update(saved)
dev.link = True; dev.phy[0x11] = 0xad02
check('back to the AX88179 PHY: 1000FD from register 0x11 -> 0x013b', call(0x13aa0418, P) == 0 and macreg(0x22, 2) == 0x13b, hex(macreg(0x22, 2)))

# link drop -> RX thread stop path (FUN_13aa0230 -> FUN_13aa1878 -> swreset), then link up at 100M
dev.link = False
check('link lost -> -0x18', call(0x13aa0418, P) == -0x18)
uc.mem_write(P + 0x80, b'\x01')
call(0x13aa0230, P)
check('rx-thread stop: PHYPWR = IPRL|BZ (0x30)', macreg(0x26, 2) == 0x30, hex(macreg(0x26, 2)))
dev.link = True; dev.phy[0x11] = 0x6400             # 100M full duplex
r = call(0x13aa0418, P)
check('link up 100FD -> medium 0x0332, BZ cleared', r == 0 and macreg(0x22, 2) == 0x332 and macreg(0x26, 2) == 0x20,
      (r, hex(macreg(0x22, 2)), hex(macreg(0x26, 2))))
dev.link = False; call(0x13aa0418, P); dev.link = True; dev.phy[0x11] = 0x0400   # 10M half
check('link up 10HD -> medium 0x0130', call(0x13aa0418, P) == 0 and macreg(0x22, 2) == 0x130)

# ---- 4. get / set MAC (ioctl entries) ----
call(S['ax179_getmac'], CTX, 0x13ab3100)
check('getmac', bytes(uc.mem_read(0x13ab3100, 6)).hex() == '000ec6e40619')
uc.mem_write(0x13ab3200, bytes.fromhex('020000aabbcc'))
check('setmac', call(S['ax179_setmac'], CTX, 0x13ab3200) >= 0 and dev.mac[0x10:0x16].hex() == '020000aabbcc')

# ---- 5. interface down (FUN_13aa03c8 -> stop), then what the main loop does next ----
med0, rxc0 = macreg(0x22, 2), macreg(0x0b, 2)
call(0x13aa03c8, P)
check('down: device state = 1', r8(P + 1) == 1, r8(P + 1))
check('down: only RECEIVE_EN cleared in MEDIUM (speed/duplex kept), rx_ctl untouched, PHY powered off',
      macreg(0x22, 2) == med0 & ~0x100 and macreg(0x0b, 2) == rxc0 and macreg(0x26, 2) == 0,
      (hex(med0), hex(macreg(0x22, 2)), hex(macreg(0x0b, 2)), hex(macreg(0x26, 2))))
# main loop: link poll in state 1 -> -0x18 -> link flag (+0x4c) was set -> FUN_13aa0230 stops the threads
check('link poll after down -> -0x18', call(0x13aa0418, P) == -0x18)
dev.rx_released = False; w32(P + 0x78, RXTID); uc.mem_write(P + 0x80, b'\x01')
try:
    call(0x13aa0230, P); hang = None
except Hang as e:
    hang = str(e)
check('thread stop after down: forceread releases the RX thread (join returns)', hang is None, hang)
check('thread stop after down: PHY back in reset, port stays down (PHYPWR 0)', macreg(0x26, 2) == 0, hex(macreg(0x26, 2)))
dev.phy[9] = 0x0200; n9 = len(dev.log)                 # the PHY reset brought the default back
call(0x13aa0358, P)
check('up again after down: 1000BASE-T not advertised again', (dev.phy[9] & 0x0300) == 0 and reg9_before_an(n9) if not GIGABIT else dev.phy[9] == 0x0200, hex(dev.phy[9]))
check('up again after down: state 2, PHY on, rx_ctl 0x0298', r8(P + 1) == 2 and macreg(0x26, 2) == 0x20 and macreg(0x0b, 2) == 0x298)
dev.link = True; dev.phy[0x11] = 0xad02
check('link after up again: 1000FD medium 0x013b', call(0x13aa0418, P) == 0 and macreg(0x22, 2) == 0x13b, hex(macreg(0x22, 2)))

# ---- 6. only AX88179 vendor requests were used ----
check('no AX88772 requests issued', all(e[0] not in ('W', 'R') or e[1] in (1, 2) for e in dev.log))
check('AX88179: no endpoint cancel used', cancels == [], cancels)

# ---- 6b. AX88179A in CDC-ECM mode (configuration 3), same module flow ----
class EcmDev:
    """AX88179A as seen by the diagnostics: 3 configurations, MAC string '6C6E072362A' + NUL"""
    def __init__(s):
        s.config = 1; s.alt = 0; s.filter = None; s.log = []; s.delay_us = 0
        s.bulk_in = []; s.bulk_out = []; s.rx_released = False
    def ctrl(s, fd, ep0, bm, req, val, idx, ln, buf):
        assert fd == FD and ep0 == EP0H, (hex(fd), hex(ep0))
        s.log.append((bm, req, val, idx, ln))
        if bm == 0x80 and req == 6 and val == 0x0100:
            uc.mem_write(buf, bytes.fromhex('120110020000004095 0b9017000201020303'.replace(' ', ''))[:ln]); return ln
        if bm == 0x80 and req == 6 and val == 0x0305:
            assert idx == 0x0409
            d = bytes.fromhex('1a03360043003600450030003700320033003600320041000000')[:ln]
            uc.mem_write(buf, d); return len(d)
        if bm == 0x80 and req == 8: uc.mem_write(buf, bytes([s.config])); return 1
        if bm == 0x00 and req == 9: s.config = val; s.alt = 0; s.notes = 3; return 0
        if bm == 0x01 and req == 0x0b: assert idx == 1; s.alt = val; return 0
        if bm == 0x21 and req == 0x43: assert s.config == 3; s.filter = val; return 0
        raise Exception('unexpected request in ECM mode: %r' % (s.log[-1],))
    def intr(s, buf, ln, ep):
        assert ep == 0x81 and ln == 16
        if s.notes <= 0: raise Hang('interrupt endpoint read in ECM mode (blocks until a link change)')
        s.notes -= 1; s.nread += 1
        d = [bytes.fromhex('a100000001000000'), bytes.fromhex('a12a000001000800'), bytes.fromhex('a100010001000000')][2 - s.notes]
        uc.mem_write(buf, d); return len(d)
dev = EcmDev(); dev.notes = 0; dev.nread = 0; cancels.clear()
uc.mem_write(P, b'\0' * 0x200)
w32(P + 8, FD); w32(P + 0xc, EP0H)
for o in (0x74, 0x78, 0x84, 0x98): w32(P + o, 0xffffffff)
w32(P + 0x50, 0); w32(P + 0x54, 3); uc.mem_write(P + 0x58, b''.join(mcl))
w32(0x13aa4100, 0)                              # FUN_13aa1810 "initialised" flag (cleared by the close path FUN_13aa1960)
call(0x13aa0454, P)
check('ECM open: state 1, configuration 3, data IF alt 1', r8(P + 1) == 1 and dev.config == 3 and dev.alt == 1, (r8(P + 1), dev.config, dev.alt))
check('ECM open: the 3 notifications were read after the filter, none left', dev.nread == 3 and dev.notes == 0, (dev.nread, dev.notes))
check('ECM open: filter 0x000f, settle delay 1 s', dev.filter == 0x0f and dev.delay_us == 1000000, (dev.filter, dev.delay_us))
check('ECM open: MAC 6c6e072362a0 (from the string, last digit missing -> 0)', bytes(uc.mem_read(CTX + 0xc, 6)).hex() == '6c6e072362a0',
      bytes(uc.mem_read(CTX + 0xc, 6)).hex())
check('ECM open: only standard/class requests', all(e[0] in (0x80, 0x00, 0x01, 0x21) for e in dev.log))
li = bytes(uc.mem_read(P + 0x34, 12))
check('ECM open: link info 100 FD AN', struct.unpack('>IIH', li[:10]) == (0x4f, 0x4f, 100) and li[10:12] == b'\x01\x01', li.hex())
n0 = len(dev.log)
call(0x13aa0358, P)
check('ECM up: state 2, no second SET_CONFIGURATION / SET_INTERFACE', r8(P + 1) == 2 and
      not any(e[0] in (0x00, 0x01) for e in dev.log[n0:]) and dev.delay_us == 1000000, dev.log[n0:])
check('ECM up: rx state keeps the ECM flag', r32(CTX + 0x3c) == 0x80000000 and r32(CTX + 0x38) == 0, hex(r32(CTX + 0x3c)))
check('ECM link poll -> 0 without interrupt read', call(0x13aa0418, P) == 0)
w32(0x13ab3000, 0x18); w32(0x13ab3004, 0)
check('ECM rxmode -> 0, filter stays 0x000f', call(S['ax179_rxmode'], CTX, 0x13ab3000) == 0 and dev.filter == 0x0f)
call(S['ax179_getmac'], CTX, 0x13ab3100)
check('ECM getmac', bytes(uc.mem_read(0x13ab3100, 6)).hex() == '6c6e072362a0')
uc.mem_write(0x13ab3200, bytes.fromhex('020000aabbcc'))
n0 = len(dev.log)
check('ECM setmac ignored (no request), getmac keeps the adapter address', call(S['ax179_setmac'], CTX, 0x13ab3200) == 0 and len(dev.log) == n0
      and call(S['ax179_getmac'], CTX, 0x13ab3100) == 0 and bytes(uc.mem_read(0x13ab3100, 6)).hex() == '6c6e072362a0')
# TX: raw frame, one pad byte for multiples of 512
IOB, FR = 0x13ab3400, 0x13ab3500
for ln in (60, 511, 512, 1024, 1514):
    fr = bytes((i * 7 + ln) & 0xff for i in range(ln))
    uc.mem_write(IOB, b'\0' * 0x40); w32(IOB + 0x10, FR); w16(IOB + 0x16, ln); uc.mem_write(FR, fr + b'\xee')
    dev.bulk_out = []
    r = call(S['ax179_tx'], CTX, IOB)
    exp = fr + (b'\xee' if ln % 512 == 0 else b'')
    check(f'ECM TX {ln} bytes -> raw frame{" + 1 pad byte" if ln % 512 == 0 else ""}, iob untouched',
          r == 0 and dev.bulk_out[-1:] == [exp] and r32(IOB + 0x10) == FR and r16(IOB + 0x16) == ln, (r, [len(x) for x in dev.bulk_out]))
# RX: one frame per transfer, frame starts at the buffer
OUT, LEN = 0x13ab3300, 0x13ab3310
frames = [bytes.fromhex('ffffffffffffa842a18371e288992580') + bytes(44), bytes(range(256)) * 5 + bytes(234)]
dev.bulk_in = list(frames) + [0, 5, -4]
for f in frames:
    r = call(S['ax179_rx'], CTX, OUT, LEN)
    check(f'ECM RX {len(f)} bytes', r == 0 and r32(OUT) == 0x13aa4120 and bytes(uc.mem_read(r32(OUT), r16(LEN))) == f,
          (r, hex(r32(OUT)), r16(LEN)))
w16(LEN, 0x5555); r = call(S['ax179_rx'], CTX, OUT, LEN)
check('ECM RX reads with 1536 bytes', dev.last_in_len == 1536, getattr(dev, 'last_in_len', None))
check('ECM RX zero-length transfer -> 0, len 0', r == 0 and r16(LEN) == 0)
w16(LEN, 0x5555); r = call(S['ax179_rx'], CTX, OUT, LEN)
check('ECM RX runt (5 bytes) -> 0, len 0', r == 0 and r16(LEN) == 0)
# TX in ECM mode sends only the real frame, RX leaves the rest of the buffer alone
w32(CTX + 0x3c, 0x80000000); uc.mem_write(0x13aa4920, bytes(16))
dev.bulk_in = [bytes(1514), -4, bytes(590)]
for _ in range(3): call(S['ax179_rx'], CTX, OUT, LEN)
dev.bulk_out = []
IOB2, FR2 = 0x13ab3600, 0x13ab3700
uc.mem_write(IOB2, b'\0' * 0x40); w32(IOB2 + 0x10, FR2); w16(IOB2 + 0x16, 60); uc.mem_write(FR2, bytes(60))
check('ECM TX: ok', call(S['ax179_tx'], CTX, IOB2) == 0)
fr = dev.bulk_out
check('ECM TX sends only the real frame', len(fr) == 1 and len(fr[0]) == 60 and fr[0][12:14] != b'\x88\xb5', [len(x) for x in fr])
check('ECM RX does not touch the buffer tail', bytes(uc.mem_read(0x13aa4920, 16)) == bytes(16))
dev.bulk_out = []; dev.bulk_in = [bytes(100)]; call(S['ax179_rx'], CTX, OUT, LEN)
check('ECM RX sends nothing', dev.bulk_out == [])
dev.bulk_in = []
w32(CTX + 0x3c, 0x80000000); dev.bulk_in = [-4]
check('ECM RX error passed on', call(S['ax179_rx'], CTX, OUT, LEN) == -4)
dev.bulk_in = []

# down -> link poll -0x18 -> thread stop: swreset must cancel the pending bulk-in
n0 = len(dev.log)
call(0x13aa03c8, P)
check('ECM down: state 1, no requests', r8(P + 1) == 1 and len(dev.log) == n0, dev.log[n0:])
check('ECM link poll after down -> -0x18', call(0x13aa0418, P) == -0x18)
dev.rx_released = False; w32(P + 0x78, RXTID); uc.mem_write(P + 0x80, b'\x01')
try:
    call(0x13aa0230, P); hang = None
except Hang as e:
    hang = str(e)
check('ECM thread stop: bulk-in 0x82 cancelled, join returns', hang is None and cancels == [0x82], (hang, cancels))
n0 = len(dev.log)
call(0x13aa0358, P)
check('ECM up again: state 2, config untouched, link up', r8(P + 1) == 2 and dev.config == 3 and dev.alt == 1 and
      not any(e[0] in (0x00, 0x01) for e in dev.log[n0:]) and call(0x13aa0418, P) == 0)
# adapter re-enumerated by IOS (back in configuration 1): next init switches again
dev.config = 1; dev.alt = 0; dev.delay_us = 0; dev.nread = 0
call(S['ax179_init'], CTX, P + 0x34)
check('ECM init after re-enumeration: configuration 3 again, 1 s settle, 3 notifications read', dev.config == 3 and dev.alt == 1 and dev.delay_us == 1000000 and dev.nread == 3, dev.nread)
# string descriptor not readable -> fallback address 02:00:00:00:00:00
orig_ctrl = dev.ctrl
def ctrl_nostr(*a):
    if a[2] == 0x80 and a[3] == 6 and a[4] == 0x0305: return -4
    return orig_ctrl(*a)
dev.ctrl = ctrl_nostr
call(S['ax179_init'], CTX, P + 0x34)
check('ECM init without MAC string: 020000000000', bytes(uc.mem_read(CTX + 0xc, 6)).hex() == '020000000000',
      bytes(uc.mem_read(CTX + 0xc, 6)).hex())
dev.ctrl = orig_ctrl

# ---- 7. RX / TX of generated frames through the patched module ----
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
w32(CTX + 0x3c, 0)                                    # AX88179 mode
OUT, LEN = 0x13ab3300, 0x13ab3310
tests = [[frame(60, 1)], [frame(1514, 2)], [frame(n, n) for n in (60, 98, 342, 590, 1514)], [frame(64, 3), frame(12, 4), frame(70, 5)]]
good = 0
for k, frs in enumerate(tests):
    bad = (1,) if k == 2 else ()
    call(S['ax179_rxreset'], CTX); dev.bulk_in = [agg(frs, bad)]
    exp = [f for i, f in enumerate(frs) if i not in bad and len(f) >= 14]
    got = []
    for _ in exp:
        r = call(S['ax179_rx'], CTX, OUT, LEN)
        got.append(bytes(uc.mem_read(r32(OUT), r16(LEN))) if r == 0 else None)
    good += got == exp
check(f'RX {good}/{len(tests)} aggregated transfers (1 and 5 frames, CRC error and runt skipped)', good == len(tests))
def tx_ref(f):
    pad = 0x80 if (len(f) + 8) % 512 == 0 else 0
    return struct.pack('<I', len(f)) + bytes([0, pad, 0, pad]) + f
IOB, FR = 0x13ab3400, 0x13ab3500
outs = [tx_ref(frame(n, n)) for n in (42, 60, 342, 504, 1016, 1514)]     # 504 / 1016: total 512 / 1024
okt = 0
for o in outs:
    dev.bulk_out = []
    uc.mem_write(IOB, b'\0' * 0x40); w32(IOB + 0x10, FR); w16(IOB + 0x16, len(o) - 8); uc.mem_write(FR, o[8:])
    okt += call(S['ax179_tx'], CTX, IOB) == 0 and dev.bulk_out == [o]
check(f'TX {okt}/{len(outs)} frames byte-exact (header, 512-byte padding flag)', okt == len(outs))

print('ALL OK' if ok else 'FAILURES')
sys.exit(0 if ok else 1)
