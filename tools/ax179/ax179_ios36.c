// AX88179 / AX88179A driver for the ETH module of IOS36 v3864 (content 0x0a)
//
// What is this file?
//   The ETH module of an IOS contains Nintendo's driver for the AX88772 USB adapter. This file is
//   the same kind of driver for the AX88179 (Nintendo Switch LAN adapter) and the AX88179A.
//   It is compiled to machine code, and build_patch36.py copies that code over the old driver.
//   You do NOT need to build anything to use the patcher: the finished result is already
//   in source/ax179_ios36_data.h.
//   (The same module is in IOS31, 33, 34, 35, 37, 38, 41, 43, 45, 46, 48, 53, 55, 56, 62 and 80.)
//
// How does it fit into the module?
//   - This module is ARM code (the IOS57/58/59 modules are Thumb). The driver is Thumb code, the
//     two kinds of code can call each other.
//   - The old driver is at 0x13aa0084..0x13aa1290 (axLockAccessToPhy .. axInit) and
//     0x13aa13c8..0x13aa1500 (axForceReadCompletion, axDown). The new code is placed there.
//     axOpen / axClose only open and close the USB device and stay.
//   - Every function marked SEC("e_...") below is an "entry point". It does the same job as one
//     function of the old driver and takes the same arguments, so build_patch36.py only has to
//     change the calls of the module to point at it. The comment above each entry point names
//     the old function it replaces.
//   - The same chip sequences are used as in ax179_drv.c (the IOS58 driver).
//
// Things to know when reading the code
//   - The CPU is ARM926EJ-S and big-endian. The USB chip is little-endian, so register values
//     and packet headers are put together byte by byte.
//   - There is no operating system, no libc and no global variables. Everything the driver needs
//     is either passed in (ctx) or already exists in the module (the functions below).
//   - Two adapters: the AX88179 is driven with its vendor registers, the AX88179A in CDC-ECM mode
//     (plain ethernet frames, see ecm_init()).
//   - Some code is only there for debugging (counters, "ring" event log). It does not change how
//     the adapter works; see the notes at N_TX and at the ring.

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef int            s32;

// ---- functions and variables that already exist in the module (ARM veneers / syscall stubs) ----
#define CTRL     ((s32 (*)(u32, u32, u32, u32, u32, u32, void *))0x13aa5130)  // USB control transfer: fd, bmRequestType, bRequest, wValue, wIndex, wLength, buffer
#define INTR     ((s32 (*)(u32, u32, u32, void *))0x13aa5150)                 // USB interrupt-in: fd, endpoint, length, buffer
#define BULK_OUT ((s32 (*)(u32, u32, u32, void *))0x13aa5160)                 // USB bulk out: fd, endpoint, length, buffer
#define BULK_IN  ((s32 (*)(u32, u32, u32, void *))0x13aa5170)                 // USB bulk in: fd, endpoint, length, buffer
#define SLEEP    ((s32 (*)(u32))0x13aa5180)                                   // wait for some microseconds
#define IOB_PUSH ((u8 *(*)(void *, u32))0x13aa4e78)                           // IOS_PushIob: make room for a header in front of the data
#define IOB_PULL ((s32 (*)(void *, u32))0x13aa4e80)                           // IOS_PullIob: remove it again
#define FLUSH    ((void (*)(const void *, u32))0x13aa4ea0)                    // syscall 0x40, flush the data cache
#define RXMAX    (*(volatile u32 *)0x13ab6b60)                                // size of one bulk-in transfer

// ---- the "ctx" structure the module passes to every entry point (at dev + 0x94) -------------
// Words (u32 index):
#define CTX_FD         0       // handle of the USB device (set by axOpen, cleared by axClose)
#define CTX_LINK_UP    1       // 1 = link is configured
#define CTX_FLAGS      2       // bit 0: PHY is powered down, bits 8..15: counter, bit 31: ECM mode
#define CTX_RX_BUFFER  0xd     // buffer for the received data
#define CTX_RX_HEADERS 0xe     // AX88179 receive state: where the frame headers are | count << 16
#define CTX_RX_POS     0xf     // AX88179 receive state: offset of the next frame | its number << 16
// Bytes:
#define CTX_MAC_BYTE   0xc     // the 6 bytes of the MAC address (same place as in the original driver)
// The words 5..11 are the unused "EEPROM copy" of the AX88772 driver. axOpen clears them.
// Word 0xc is owned by axOpen / axClose.

#define LINKFLAG(ctx) ((ctx)[CTX_LINK_UP])
#define FLAGS(ctx)    ((ctx)[CTX_FLAGS])

// ---- the "li" structure (link info) ---------------------------------------------------------
// [0] abilities, [1] advertised abilities (bit 6 autoneg, bits 3..0 = 100 full/half, 10 full/half)
// u16 at byte 8: speed in Mbit/s, byte 10: 1 = full duplex, byte 11: 1 = autoneg on
#define LI_SPEED(li)   (((u16 *)(li))[4])
#define LI_DUPLEX(li)  (((u8 *)(li))[10])
#define LI_AUTONEG(li) (((u8 *)(li))[11])

// ---- AX88179 vendor requests and registers --------------------------------------------------
#define REQ_MAC 1                  // read / write a register of the MAC
#define REQ_PHY 2                  // read / write a register of the PHY
#define PHY_ID  3

#define R_RX_CTL      0x0b         // which frames are received
#define R_NODE_ID     0x10         // MAC address
#define R_MULFLTARY   0x16         // multicast hash filter (8 bytes)
#define R_MEDIUM      0x22         // speed, duplex, receive enable
#define R_PHYPWR      0x26         // PHY power and reset
#define R_BULKIN_Q    0x2e         // how much data is gathered into one bulk-in transfer
#define R_CLK_SELECT  0x33
#define R_RXCOE       0x34         // receive checksum offload
#define R_TXCOE       0x35         // transmit checksum offload
#define R_PAUSE_HIGH  0x54
#define R_PAUSE_LOW   0x55

#define RX_CTL_BASE   0x0298      // IPE | START | AM | AB (as in the capture)
#define RX_CTL_INIT   0x03aa      // what linux writes during reset
#define MEDIUM_BASE   0x0130      // RECEIVE_EN | TXFLOW | RXFLOW
#define MEDIUM_INIT   0x0133      // + FULL_DUPLEX | GIGAMODE before the link is up
#define MEDIUM_RECEIVE_EN 0x0100
#define PHYPWR_IPRL   0x0020
#define PHYPWR_BZ     0x0010      // bulk-in returns zero length (unblocks the rx thread)
#define BULKIN_SIZE_KB 2          // same as the IOS58 driver

#define EP_OUT 3                   // bulk out (we send)
#define EP_IN  0x82                // bulk in (we receive)
#define EP_INT 0x81                // interrupt in (link status)
#define HS_MAXPKT_MASK 511         // USB 2.0 high speed: packets are 512 bytes long
#define RXH_ERR_MASK   0xA0000000u // CRC_ERR (bit 29) | DROP_ERR (bit 31)

// Debug counters in the unused EEPROM copy of ctx. Nothing reads them, they are only handy when
// looking at a memory dump.
#define N_TX(ctx)     ((ctx)[5])    // ax179_tx calls
#define N_PUSH8F(ctx) ((ctx)[6])    // IOS_PushIob(8) failed
#define N_FALLB(ctx)  ((ctx)[7])    // frames sent through the copy path
#define N_LASTRC(ctx) ((ctx)[8])    // last BULK_OUT result of ax179_tx
#define N_RX(ctx)     ((ctx)[9])    // ax179_rx calls
#define N_RXF(ctx)    ((ctx)[10])   // frames handed to the stack
#define N_LINK(ctx)   ((ctx)[11])   // ax179_link calls

#define PHY_IS_OFF(ctx)   (FLAGS(ctx) & 1u)
#define N_SHORT(ctx)      (FLAGS(ctx) >> 8)   // short bulk-in transfers (counts up to 8)

// Puts a function into its own section, so that block36.ld can place it and keep it.
#define SEC(x) __attribute__((section(".text." x), used))

// reads 4 bytes of a little-endian buffer
static inline __attribute__((always_inline)) u32 le32(const u8 *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
}

// ---- register access (AX88179) --------------------------------------------------------------
// The buffers are on the stack, like in the original driver.

// write len bytes, the value is little-endian
static s32 wr(u32 fd, u32 req, u32 val, u32 idx, u32 len, u32 data)
{
    u8 s[4] __attribute__((aligned(4)));
    for (u32 i = 0; i < len; i++)
        s[i] = data >> (8 * i);
    return CTRL(fd, 0x40, req, val, idx, len, s);
}

// read 2 bytes, returns the value or a negative error
static s32 rd(u32 fd, u32 req, u32 val, u32 idx)
{
    u8 s[4] __attribute__((aligned(4)));
    s32 r = CTRL(fd, 0xc0, req, val, idx, 2, s);
    if (r < 0)
        return r;
    return s[0] | (s[1] << 8);
}
#define MAC_W(fd, reg, len, v) wr(fd, REQ_MAC, reg, len, len, v)
#define PHY_W(fd, reg, v)      wr(fd, REQ_PHY, PHY_ID, reg, 2, v)
#define PHY_R(fd, reg)         rd(fd, REQ_PHY, PHY_ID, reg)

// sets how much the chip gathers into one bulk-in transfer
static s32 bulkin_cfg(u32 fd)
{
    u8 s[8] __attribute__((aligned(4)));
    s[0] = 0x07; s[1] = 0x20; s[2] = 0x03; s[3] = BULKIN_SIZE_KB; s[4] = 0xff;
    return CTRL(fd, 0x40, REQ_MAC, R_BULKIN_Q, 5, 5, s);
}

// Reads the MEDIUM bits for the current link out of PHY register 0x11
// (bit 10 link, bits 15:14 speed, bit 13 duplex).
static s32 phy_medium(u32 fd)
{
    s32 st = PHY_R(fd, 0x11);
    if (st < 0)
        return st;

    u32 mode = MEDIUM_BASE;
    if (st & 0x0400) {                                   // link is up
        if ((st & 0xc000) == 0x8000)
            mode |= 0x0009;                              // 1000M: GIGAMODE | EN_125MHZ
        else if ((st & 0xc000) == 0x4000)
            mode |= 0x0200;                              // 100M: PS
        if (st & 0x2000)
            mode |= 0x0002;                              // FULL_DUPLEX
    }
    return mode;
}

// 1000BASE-T is not advertised (PHY register 9, bits 8/9 cleared), autonegotiation only offers 10/100.
// On a Wii in WiiConnect24 standby the link came up at 1000M for a moment, dropped and settled at
// 100M, and until then the System Menu failed with error 51420. The Wii (USB 2.0) gains nothing from
// 1000M. It has to be written before every autonegotiation restart, because a PHY reset (ax179_down)
// brings the default back. Build with GIGABIT=1 to keep the chip default.
#ifndef GIGABIT
#define GIGABIT 0
#endif
static void no_gigabit(u32 fd)
{
#if GIGABIT
    (void)fd;
#else
    s32 c = PHY_R(fd, 9);
    if (c >= 0)
        PHY_W(fd, 9, (u32)c & ~0x0300u);
#endif
}

// the link just came up: take the PHY out of reset and copy speed and duplex into MEDIUM
static s32 link_reset(u32 fd)
{
    s32 r = MAC_W(fd, R_PHYPWR, 2, PHYPWR_IPRL);        // clears BZ left by forceread
    if (r < 0)
        return r;
    s32 mode = phy_medium(fd);
    if (mode < 0)
        return mode;
    return MAC_W(fd, R_MEDIUM, 2, (u32)mode);
}

// ---- AX88179A in CDC-ECM mode ---------------------------------------------------------------
// USB configuration 3, same design as the IOS58 driver: raw ethernet frames on bulk 0x82 / 0x03.
// ECM mode is remembered with ECM_FLAG in CTX_FLAGS.
#define ECM_FLAG    0x80000000u
#define ECM_CONFIG  3
#define ECM_RXLEN   1536
#define ECM_FILTER  0x000f   // promiscuous | all multicast | directed | broadcast
#define IS_ECM(ctx) (FLAGS(ctx) & ECM_FLAG)

// Link timing like the AX88179. The module's "up" sends one poll message and only then sets
// its state to 2; the poll handler re-arms its 1 s timer only in state 2. The AX88179 link
// status is an interrupt-in read that takes a while, so the state is 2 by then - an instant
// answer loses that race and polling stops for good. So every ECM poll waits ECM_POLL_US, and
// the link is reported down for the first ECM_LINK_POLLS polls after init / up (~3 s), so that
// LINK UP reaches the stack after SO startup, as with the AX88179's autonegotiation.
// Otherwise KD sees "IP set, link flag 0" and resets the socket layer.
// The poll counter is in CTX_FLAGS, bits 8..15.
#define ECM_POLL_US    100000
#define ECM_LINK_POLLS 4
#define ECM_POLLS(ctx)      ((FLAGS(ctx) >> 8) & 0xff)
#define ECM_POLLS_CLEAR(ctx) (FLAGS(ctx) &= ~0xff00u)
#define ECM_POLLS_ADD(ctx)  (FLAGS(ctx) += 0x100)

// ---- ECM event log (debugging only) ---------------------------------------------------------
// A small log of what happened in ECM mode, kept in the unused part of the rx buffer (ECM only
// uses its first 1536 bytes): the module's static rx buffer CTX_RX_BUFFER at 0x13aa9020.
// Nothing in the patcher reads it. It costs a few bytes and helps when a memory dump of an
// ECM problem is needed.
//
// The log is at RING_OFF in the buffer: 16 header words, then RING_N entries of 2 words each.
// Header words (RW_...) count calls and keep the last result.
// An entry is: code << 24 | (link polls & 0xff) << 16 | (event number & 0xffff), then a value.
#define RING_OFF   0x800
#define RING_N     128
#define RING_WORDS (16 + 2 * RING_N)
#define RMAGIC     0x52333700u                                      // 'R37\0'
#define RW_MAGIC       0       // magic number: the log is set up
#define RW_EVENTS      1       // number of events so far
#define RW_CTX         2       // address of ctx
#define RW_LINK_POLLS  3       // ax179_link calls
#define RW_TX_CALLS    4       // ax179_tx calls (ECM)
#define RW_TX_RC       5       // last tx result
#define RW_RX_CALLS    6       // ax179_rx calls (ECM)
#define RW_RX_LAST     7       // last rx result
#define RW_LINK_LAST   8       // last link result
#define RW_UP         10       // ax179_up calls
#define RW_DOWN       11       // ax179_down calls
#define RW_FORCEREAD  12       // ax179_forceread calls
#define RW_RXMODE     13       // ax179_rxmode calls
// event codes
enum { L_INIT = 1, L_INITDONE, L_UP, L_DOWN, L_FORCEREAD, L_RXMODE, L_LINK, L_GETMAC, L_SETMAC,
       L_TXERR, L_RXCHG, L_MAC0, L_MAC1, L_MACSTR, L_SPEED };

// returns the log (and sets it up the first time)
static u32 *ring(u32 *ctx)
{
    u32 *r = (u32 *)(ctx[CTX_RX_BUFFER] + RING_OFF);
    if (r[RW_MAGIC] != RMAGIC) {
        for (u32 i = 0; i < RING_WORDS; i++)
            r[i] = 0;
        r[RW_MAGIC] = RMAGIC;
        r[RW_LINK_LAST] = 0x7fffffff;
    }
    r[RW_CTX] = (u32)ctx;
    return r;
}

// writes one event into the log
__attribute__((noinline)) static void rlog(u32 *ctx, u32 code, u32 val)
{
    if (!ctx[CTX_RX_BUFFER])
        return;
    u32 *r = ring(ctx);
    u32 n = r[RW_EVENTS]++;
    u32 *e = r + 16 + 2 * (n % RING_N);
    e[0] = code << 24 | (r[RW_LINK_POLLS] & 0xff) << 16 | (n & 0xffff);
    e[1] = val;
    FLUSH(r, RING_WORDS * 4);
}
// counts one call in a header word
#define RCOUNT(ctx, i) do { if ((ctx)[CTX_RX_BUFFER]) ring(ctx)[i]++; } while (0)

static s32 ecm_filter(u32 fd, u8 *b)
{
    return CTRL(fd, 0x21, 0x43, ECM_FILTER, 0, 0, b);          // SET_ETHERNET_PACKET_FILTER
}

// Starts the AX88179A. Same steps as ecm_init() of the IOS58 driver: switch to configuration 3
// (only if it is not active yet), set the filter, take the MAC address from USB string 5.
// b is a buffer for the USB answers.
static s32 ecm_init(u32 *ctx, u32 *li, u8 *b)
{
    u32 fd = ctx[CTX_FD];
    s32 r = CTRL(fd, 0x80, 8, 0, 0, 1, b);                      // GET_CONFIGURATION
    rlog(ctx, L_INIT, r < 0 ? (u32)r : b[0]);
    if (r < 0)
        return r;

    u32 must_switch = b[0] != ECM_CONFIG;
    if (must_switch) {
        r = CTRL(fd, 0x00, 9, ECM_CONFIG, 0, 0, b);             // SET_CONFIGURATION
        if (r < 0)
            return r;
        r = CTRL(fd, 0x01, 0x0b, 1, 1, 0, b);                   // SET_INTERFACE 1 alt 1
        if (r < 0)
            return r;
        SLEEP(1000000);
    }
    r = ecm_filter(fd, b);
    if (r < 0)
        return r;

    // After a switch, read the 3 notifications (disconnected, speed, connected). In ECM mode the
    // adapter negotiates its link speed itself (no PHY access), so the speed it reports
    // (ConnectionSpeedChange 0x2a, downstream bit/s) is only logged; to keep it at 100M the
    // switch / router port has to be limited.
    if (must_switch) {
        for (int i = 0; i < 3; i++) {
            if (INTR(fd, EP_INT, 16, b) < 0)
                break;
            if (b[1] == 0x2a)
                rlog(ctx, L_SPEED, le32(b + 8));
        }
    }

    // The MAC address is a string descriptor in UTF-16: 12 hex digits, one every 2 bytes,
    // starting at byte 2 (after length and type). Every 2 digits make one byte of the address.
    u8 *mac = (u8 *)ctx + CTX_MAC_BYTE;
    r = CTRL(fd, 0x80, 6, 0x0305, 0x0409, 26, b);               // string number 5 = iMACAddress
    if (r < 0)
        b[2] = 0;                                               // no string: no digits at all
    rlog(ctx, L_MACSTR, (u32)r << 16 | b[0] << 8 | b[2]);
    // 'end' becomes 1 at the first character that is no hex digit, the digits after it are 0
    for (u32 i = 0, end = 0; i < 12; i++) {
        u32 ch = b[2 + 2 * i];
        u32 digit = ch - '0';
        if (digit > 9)
            digit = (ch | 0x20) - 'a' + 10;                     // a..f (and A..F)
        if (digit > 15)
            end = 1;
        if (end)
            digit = 0;
        mac[i >> 1] = (i & 1) ? mac[i >> 1] | digit             // odd digit: low half of the byte
                              : digit << 4;                     // even digit: high half of the byte
    }
    if (!(mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]))
        mac[0] = 0x02;                                          // no string: a "local" address
    rlog(ctx, L_MAC0, ctx[3]);
    rlog(ctx, L_MAC1, ctx[4]);

    // tell the module what the link looks like
    li[0] = 0x4f;                                               // autoneg, 100/10 full/half
    li[1] = 0x4f;
    LI_SPEED(li) = 100;
    LI_DUPLEX(li) = 1;
    LI_AUTONEG(li) = 1;
    ECM_POLLS_CLEAR(ctx);
    LINKFLAG(ctx) = 1;
    rlog(ctx, L_INITDONE, must_switch);
    return 0;
}

// The PHY comes back from power-down (ax179_down): the same steps as in init, without the MAC part
static void phy_wake(u32 *ctx)
{
    u32 fd = ctx[CTX_FD];
    MAC_W(fd, R_PHYPWR, 2, PHYPWR_IPRL);
    SLEEP(200000);
    MAC_W(fd, R_CLK_SELECT, 1, 0x03);
    SLEEP(100000);
    PHY_W(fd, 0x0d, 0x0007); PHY_W(fd, 0x0e, 0x003c);          // EEE advertisement off again
    PHY_W(fd, 0x0d, 0x4007); PHY_W(fd, 0x0e, 0x0000);
    FLAGS(ctx) &= ~1u;                                          // PHY is on again
}

// ===========================================================================================
// Entry points. Each one replaces a function of the old driver (named in the comment).
// ===========================================================================================

// replaces axInit (0x13aa0e7c): init(ctx, li) - start the adapter
// li: see "link info" above, derived from the standard MII registers like the original driver does
SEC("e_init") s32 ax179_init(u32 *ctx, u32 *li)
{
    u32 fd = ctx[CTX_FD];
    s32 r;
    LINKFLAG(ctx) = 0;
    FLAGS(ctx) &= ~ECM_FLAG;

    // The device descriptor: byte 17 is bNumConfigurations. 3 or more -> AX88179A, use CDC-ECM.
    u8 d[32] __attribute__((aligned(32)));
    if (CTRL(fd, 0x80, 6, 0x0100, 0, 18, d) >= 0 && d[17] >= ECM_CONFIG) {
        FLAGS(ctx) |= ECM_FLAG;
        return ecm_init(ctx, li, d);
    }

    // --- AX88179: reset the PHY and clocks
    r = MAC_W(fd, R_PHYPWR, 2, 0);
    if (r < 0)
        return r;
    MAC_W(fd, R_PHYPWR, 2, PHYPWR_IPRL);
    SLEEP(500000);
    MAC_W(fd, R_CLK_SELECT, 1, 0x03);                   // ACS | BCS
    SLEEP(200000);

    // --- the MAC address comes from the chip (eFuse), not from the EEPROM
    u8 mac[8] __attribute__((aligned(4)));
    r = CTRL(fd, 0xc0, REQ_MAC, R_NODE_ID, 6, 6, mac);
    if (r < 0)
        return r;
    for (int i = 0; i < 6; i++)
        ((u8 *)ctx)[CTX_MAC_BYTE + i] = mac[i];

    // --- settings of the MAC
    bulkin_cfg(fd);
    MAC_W(fd, R_PAUSE_LOW, 1, 0x34);
    MAC_W(fd, R_PAUSE_HIGH, 1, 0x52);
    MAC_W(fd, R_RXCOE, 1, 0);                           // no checksum offload
    MAC_W(fd, R_TXCOE, 1, 0);
    MAC_W(fd, R_RX_CTL, 2, RX_CTL_INIT);
    r = MAC_W(fd, R_MEDIUM, 2, MEDIUM_INIT);
    if (r < 0)
        return r;

    // --- PHY: EEE advertisement off (MMD 7, reg 0x3c), no gigabit, restart autonegotiation
    PHY_W(fd, 0x0d, 0x0007); PHY_W(fd, 0x0e, 0x003c);
    PHY_W(fd, 0x0d, 0x4007); PHY_W(fd, 0x0e, 0x0000);
    no_gigabit(fd);
    s32 bmcr = PHY_R(fd, 0);                            // basic mode control
    if (bmcr < 0)
        return bmcr;
    r = PHY_W(fd, 0, bmcr | 0x1200);                    // autoneg enable | restart autoneg
    if (r < 0)
        return r;

    // --- fill "li" from the standard MII registers
    s32 bmsr = PHY_R(fd, 1);                            // basic mode status: what the PHY can do
    s32 adv = PHY_R(fd, 4);                             // what the PHY advertises
    if (bmsr < 0)
        return bmsr;
    if (adv < 0)
        return adv;

    u32 abilities = (bmsr & 0x0008) ? 0x40 : 0;         // can autoneg
    if (bmsr & 0x4000) abilities |= 8;                  // 100 full
    if (bmsr & 0x2000) abilities |= 4;                  // 100 half
    if (bmsr & 0x1000) abilities |= 2;                  // 10 full
    if (bmsr & 0x0800) abilities |= 1;                  // 10 half

    u32 advertised = (bmcr & 0x1000) ? 0x40 : 0;        // autoneg is on
    if (adv & 0x0100) advertised |= 8;
    if (adv & 0x0080) advertised |= 4;
    if (adv & 0x0040) advertised |= 2;
    if (adv & 0x0020) advertised |= 1;

    li[0] = abilities;
    li[1] = advertised;
    LI_SPEED(li) = (bmcr & 0x2000) ? 100 : 10;
    LI_DUPLEX(li) = (bmcr >> 8) & 1;
    LI_AUTONEG(li) = (bmcr >> 12) & 1;
    return 0;
}

// replaces axUp (0x13aa0c70): up(ctx, cfg) - bring the link up. cfg has the "li" layout.
// Called after axConfigMulticast, also after axDown without a new axInit.
SEC("e_up") s32 ax179_up(u32 *ctx, const u32 *cfg)
{
    u32 fd = ctx[CTX_FD];
    s32 r;
    if (IS_ECM(ctx)) {
        u8 d[8] __attribute__((aligned(32)));
        RCOUNT(ctx, RW_UP);
        r = ecm_filter(fd, d);
        rlog(ctx, L_UP, r);
        if (r < 0)
            return -0x15;
        ECM_POLLS_CLEAR(ctx);
        LINKFLAG(ctx) = 1;
        return 0;
    }

    LINKFLAG(ctx) = 0;                                  // medium mode is set on link up
    if (PHY_IS_OFF(ctx))
        phy_wake(ctx);
    r = MAC_W(fd, R_PHYPWR, 2, PHYPWR_IPRL);            // PHY on, BZ off
    if (r < 0)
        return r;
    r = MAC_W(fd, R_MEDIUM, 2, MEDIUM_BASE);
    if (r < 0)
        return r;

    const u8 *c = (const u8 *)cfg;
    if (c[11]) {                                        // autonegotiation
        no_gigabit(fd);
        u32 ad = cfg[1];                                // what the stack wants to advertise
        if (ad & 0x0f) {
            u32 anar = 0x0401;                          // selector 802.3 + pause
            if (ad & 8) anar |= 0x0100;                 // 100 full
            if (ad & 4) anar |= 0x0080;                 // 100 half
            if (ad & 2) anar |= 0x0040;                 // 10 full
            if (ad & 1) anar |= 0x0020;                 // 10 half
            PHY_W(fd, 4, anar);
        }
        r = PHY_W(fd, 0, 0x1200);                       // autoneg enable + restart
    } else {                                            // forced speed / duplex
        u32 bmcr = 0;
        if (((const u16 *)cfg)[4] == 100)
            bmcr |= 0x2000;
        if (c[10])
            bmcr |= 0x0100;
        r = PHY_W(fd, 0, bmcr);
    }
    if (r < 0)
        return r;
    ctx[CTX_RX_HEADERS] = 0;
    ctx[CTX_RX_POS] = 0;
    return 0;
}

// replaces axDown (0x13aa1410): stop receiving and power the PHY down like the original
// (AX88772: software reset register 0x4c), so the stack and the switch port see the link go
// down; ax179_up brings the PHY back (phy_wake).
// Only RECEIVE_EN is cleared in MEDIUM, like the original axDown and linux ax88179_stop()
// (a constant would also drop GIGAMODE / FULL_DUPLEX / PS and the MAC could no longer send at
// 1000M). RX_CTL stays on: the rx thread is stopped later by ax179_forceread, and BZ only works
// while the bulk-in engine runs. axConfigMulticast rewrites RX_CTL on the next up anyway.
SEC("e_down") s32 ax179_down(u32 *ctx)
{
    u32 fd = ctx[CTX_FD];
    LINKFLAG(ctx) = 0;
    if (IS_ECM(ctx)) {
        RCOUNT(ctx, RW_DOWN);
        rlog(ctx, L_DOWN, 0);
        return 0;
    }
    s32 m = rd(fd, REQ_MAC, R_MEDIUM, 2);
    if (m < 0)
        return -0x15;
    if (MAC_W(fd, R_MEDIUM, 2, (u32)m & ~0x0100u) < 0)    // receive enable off, rest kept
        return -0x15;
    if (MAC_W(fd, R_PHYPWR, 2, 0) < 0)                    // internal PHY in reset = powered down
        return -0x15;
    FLAGS(ctx) |= 1u;                                     // remember: PHY is off
    return 0;
}

// replaces axForceReadCompletion (0x13aa13c8): make a pending bulk-in return so the rx thread
// can stop (ax179_up / link_reset clear BZ again).
// BZ goes together with IPRL (as linux does), which takes the PHY out of reset. If ax179_down
// powered it down, it goes back into reset once the zero length transfer had time to complete,
// otherwise the link comes back for a few seconds after down.
SEC("e_forceread") s32 ax179_forceread(u32 *ctx)
{
    // ECM: nothing to do here, the rx thread returns with the next frame
    if (IS_ECM(ctx)) {
        RCOUNT(ctx, RW_FORCEREAD);
        rlog(ctx, L_FORCEREAD, 0);
        return 0;
    }
    s32 r = MAC_W(ctx[CTX_FD], R_PHYPWR, 2, PHYPWR_IPRL | PHYPWR_BZ);
    if (r >= 0 && PHY_IS_OFF(ctx)) {
        SLEEP(50000);                                   // pending bulk-in completes with 0 bytes
        r = MAC_W(ctx[CTX_FD], R_PHYPWR, 2, 0);         // PHY back in reset, port stays down
    }
    return r;
}

// replaces axConfigMulticast (0x13aa07d8): rxmode(ctx, filter) - which frames are received
// filter: [1] count, then count * 6 byte addresses (the original ignores filter[0])
SEC("e_rxmode") s32 ax179_rxmode(u32 *ctx, const s32 *f)
{
    u32 fd = ctx[CTX_FD];
    u8 table[8] __attribute__((aligned(32)));
    if (IS_ECM(ctx)) {
        RCOUNT(ctx, RW_RXMODE);
        s32 r = ecm_filter(fd, table);
        rlog(ctx, L_RXMODE, (u32)f[1] << 16 | (r & 0xffff));
        return r < 0 ? -0x15 : 0;
    }

    // The chip has a 64 bit table (8 bytes) for multicast addresses. The CRC of an address
    // chooses one bit of it, the same way as the linux function ether_crc().
    *(u32 *)&table[0] = 0;
    *(u32 *)&table[4] = 0;
    const u8 *addr = (const u8 *)(f + 2);
    for (u32 n = (u32)f[1]; n; n--, addr += 6) {
        u32 crc = 0xffffffffu;
        for (int i = 0; i < 6; i++) {
            u32 d = addr[i];
            for (int k = 0; k < 8; k++, d >>= 1) {
                u32 cbit = (crc >> 31) ^ (d & 1);
                crc <<= 1;
                if (cbit)
                    crc = (crc ^ 0x04c11db7u) | 1;
            }
        }
        crc >>= 26;                                      // the upper 6 bits: bit number 0..63
        table[crc >> 3] |= 1 << (crc & 7);
    }
    if (CTRL(fd, 0x40, REQ_MAC, R_MULFLTARY, 8, 8, table) < 0)
        return -0x15;
    if (MAC_W(fd, R_RX_CTL, 2, RX_CTL_BASE) < 0)
        return -0x15;
    return 0;
}

// replaces axLinkStatus (0x13aa0914): link(ctx) - is the link up? 0 = up, -0x18 = down
SEC("e_link") s32 ax179_link(u32 *ctx)
{
    u8 st[8] __attribute__((aligned(4)));
    if (IS_ECM(ctx)) {
        // ECM has no link detection: see the note at ECM_POLL_US for the timing
        SLEEP(ECM_POLL_US);
        s32 r = -0x18;
        if (LINKFLAG(ctx)) {
            if (ECM_POLLS(ctx) >= ECM_LINK_POLLS)
                r = 0;
            else
                ECM_POLLS_ADD(ctx);
        }
        if (ctx[CTX_RX_BUFFER]) {                       // debug log
            u32 *g = ring(ctx);
            g[RW_LINK_POLLS]++;
            if ((s32)g[RW_LINK_LAST] != r) {
                g[RW_LINK_LAST] = (u32)r;
                rlog(ctx, L_LINK, r);
            }
        }
        return r;
    }

    // The chip reports the link on the interrupt endpoint. Byte 2, bit 0 is the link bit.
    s32 r = INTR(ctx[CTX_FD], EP_INT, 8, st);
    N_LINK(ctx)++;
    if (r < 0)
        return r;
    if (!(st[2] & 1)) {
        LINKFLAG(ctx) = 0;
        return -0x18;
    }
    if (!LINKFLAG(ctx)) {                               // the link was down before
        r = link_reset(ctx[CTX_FD]);
        if (r < 0)
            return r;
        LINKFLAG(ctx) = 1;
    }
    return 0;
}

// replaces 0x13aa0758 (ioctl get MAC)
SEC("e_getmac") s32 ax179_getmac(u32 *ctx, u8 *out)
{
    if (IS_ECM(ctx)) {
        for (int i = 0; i < 6; i++)
            out[i] = ((u8 *)ctx)[CTX_MAC_BYTE + i];
        rlog(ctx, L_GETMAC, ctx[4]);
        return 0;
    }
    return CTRL(ctx[CTX_FD], 0xc0, REQ_MAC, R_NODE_ID, 6, 6, out);
}

// replaces 0x13aa0798 (ioctl set MAC)
SEC("e_setmac") s32 ax179_setmac(u32 *ctx, u8 *mac)
{
    // ECM: ignored, the stack keeps the adapter's address
    if (IS_ECM(ctx)) {
        rlog(ctx, L_SETMAC, mac[2] << 24 | mac[3] << 16 | mac[4] << 8 | mac[5]);
        return 0;
    }
    return CTRL(ctx[CTX_FD], 0x40, REQ_MAC, R_NODE_ID, 6, 6, mac);
}

// replaces axXmitPacket (0x13aa097c): tx(ctx, iob) - send one frame
// iob: +0x10 pointer to the data, +0x16 length of the data (u16)
//
// The AX88179 wants an 8 byte header in front of the frame: length (LE32) and flags.
// The original only pushes 4 bytes. There are two ways to send a frame:
//   1. The IOB has room for 8 bytes: put the header in front of the data and send in place.
//   2. It has not: copy the frame into a buffer on the stack, behind a new header (up to 1520 bytes).
// ECM (AX88179A) needs no header, only a copy into an aligned buffer.
#define TXCOPY_MAX 1520
SEC("e_tx") s32 ax179_tx(u32 *ctx, u8 *iob)
{
    if (IS_ECM(ctx)) {
        u32 len = *(u16 *)(iob + 0x16);
        if (len > TXCOPY_MAX)
            return -0x16;
        u8 e[TXCOPY_MAX + 4] __attribute__((aligned(32)));
        const u8 *src = *(u8 **)(iob + 0x10);
        for (u32 i = 0; i < len; i++)
            e[i] = src[i];
        if (!(len & HS_MAXPKT_MASK))
            e[len++] = 0;                               // one pad byte instead of a ZLP
        s32 r = BULK_OUT(ctx[CTX_FD], EP_OUT, len, e);
        if (ctx[CTX_RX_BUFFER]) {                       // debug log
            u32 *g = ring(ctx);
            g[RW_TX_CALLS]++;
            if ((r < 0) != ((s32)g[RW_TX_RC] < 0))
                rlog(ctx, L_TXERR, r);
            g[RW_TX_RC] = (u32)r;
        }
        return r < 0 ? r : 0;
    }

    N_TX(ctx)++;
    s32 rc;
    u8 *h = IOB_PUSH(iob, 8);
    if (h) {
        // way 1: the header is in front of the data now
        u32 total = *(u16 *)(iob + 0x16);               // length including the header
        u32 plen  = total - 8;
        u8  pad   = (total & HS_MAXPKT_MASK) ? 0 : 0x80;     // header word 2 = 0x80008000: add a pad byte
        h[0] = plen; h[1] = plen >> 8; h[2] = 0; h[3] = 0;
        h[4] = 0;    h[5] = pad;       h[6] = 0; h[7] = pad;
        rc = BULK_OUT(ctx[CTX_FD], EP_OUT, total, h);
        if (rc >= 0)
            rc = IOB_PULL(iob, 8) ? 0 : -0x16;          // take the header off again
    } else {
        // way 2: no room for 8 bytes
        N_PUSH8F(ctx)++;
        u8 *h4 = IOB_PUSH(iob, 4);                      // the original header is 4 bytes
        if (!h4) {
            rc = -0x16;
        } else {
            u32 flen = *(u16 *)(iob + 0x16) - 4;        // length of the frame itself
            if (flen > TXCOPY_MAX) {
                IOB_PULL(iob, 4);
                rc = -0x16;
            } else {
                u8 b[8 + TXCOPY_MAX] __attribute__((aligned(32)));
                u32 total = 8 + flen;
                u8  pad = (total & HS_MAXPKT_MASK) ? 0 : 0x80;
                b[0] = flen; b[1] = flen >> 8; b[2] = 0; b[3] = 0;
                b[4] = 0;    b[5] = pad;       b[6] = 0; b[7] = pad;
                for (u32 i = 0; i < flen; i++)
                    b[8 + i] = h4[4 + i];
                rc = BULK_OUT(ctx[CTX_FD], EP_OUT, total, b);
                IOB_PULL(iob, 4);
                N_FALLB(ctx)++;
            }
        }
    }
    N_LASTRC(ctx) = (u32)rc;
    return rc < 0 ? rc : 0;
}

// replaces axRecvPacket (0x13aa0a54): rx(ctx, u8 **frame, u16 *len) - receive one frame
//
// AX88179: one bulk-in transfer holds several frames. The layout of the transfer is
//   [frame 0][frame 1] ... [list of headers][trailer: 4 bytes at the very end]
// The trailer says how many frames there are and where the list of headers starts.
// Every header (4 bytes) has the length of its frame. Each call hands out the next frame;
// the position in the transfer is kept in ctx:
//   ctx[CTX_RX_HEADERS] = where the headers start | number of frames << 16
//   ctx[CTX_RX_POS]     = offset of the next frame | its number << 16
// Each frame starts with 2 bytes that are not part of the ethernet frame.
SEC("e_rx") s32 ax179_rx(u32 *ctx, u8 **out_ptr, u16 *out_len)
{
    u8 *buf = (u8 *)ctx[CTX_RX_BUFFER];
    if (IS_ECM(ctx)) {                                  // one raw frame per transfer
        if (!(buf && RXMAX))
            return -0x16;
        s32 n = BULK_IN(ctx[CTX_FD], EP_IN, ECM_RXLEN, buf);
        u32 *g = ring(ctx);                             // debug log
        g[RW_RX_CALLS]++;
        if ((n >= 14) != ((s32)g[RW_RX_LAST] >= 14) || (n < 0 && (s32)g[RW_RX_LAST] >= 0))
            rlog(ctx, L_RXCHG, n);
        g[RW_RX_LAST] = (u32)n;
        if (n < 14) {                                   // nothing, or shorter than an ethernet header
            *out_len = 0;
            return n < 0 ? n : 0;
        }
        *out_ptr = buf;
        *out_len = (u16)n;
        return 0;
    }

    N_RX(ctx)++;
    for (;;) {
        u32 a = ctx[CTX_RX_HEADERS], b = ctx[CTX_RX_POS];
        u32 headers_at = a & 0xffff, offset = b & 0xffff, index = b >> 16;

        if (index >= (a >> 16)) {
            // All frames of the last transfer are used up: read the next transfer.
            u32 max = RXMAX;
            ctx[CTX_RX_HEADERS] = 0;
            ctx[CTX_RX_POS] = 0;
            if (!(buf && max))
                return -0x16;
            s32 n = BULK_IN(ctx[CTX_FD], EP_IN, max, buf);
            if (n < 8) {
                if (N_SHORT(ctx) < 8)
                    ECM_POLLS_ADD(ctx);                 // counts short transfers (same bits as the ECM poll counter)
                *out_len = 0;
                return n < 0 ? n : 0;
            }
            u32 trailer = le32(buf + n - 4);            // low 16 bit: number of frames, high 16 bit: where the headers start
            if ((trailer >> 16) + 4 * (trailer & 0xffff) + 4 > (u32)n)
                return -0x14;                           // the numbers do not fit into the transfer
            ctx[CTX_RX_HEADERS] = (trailer >> 16) | (trailer << 16);
            if (!(trailer & 0xffff)) {
                *out_len = 0;                           // no frames in this transfer
                return 0;
            }
            continue;
        }

        // hand out the frame number "index"
        u32 header = le32(buf + headers_at + index * 4);
        u32 length = (header >> 16) & 0x1fff;
        if (offset + length > headers_at) {             // the frame would reach into the headers
            ctx[CTX_RX_HEADERS] = 0;
            ctx[CTX_RX_POS] = 0;
            return -0x14;
        }
        ctx[CTX_RX_POS] = (offset + ((length + 7) & ~7u)) | ((index + 1) << 16);  // frames start at multiples of 8
        if ((header & RXH_ERR_MASK) || length < 2 + 14)
            continue;                                   // bad frame (CRC / dropped) or too short: next
        *out_ptr = buf + offset + 2;
        *out_len = (u16)(length - 2);
        N_RXF(ctx)++;
        return 0;
    }
}
