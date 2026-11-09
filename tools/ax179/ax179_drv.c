// AX88179 / AX88179A driver for the Thumb ETH modules of IOS58 v6432, IOS57 v6175 and IOS59 v9249
//
// What is this file?
//   The ETH module of an IOS contains Nintendo's driver for the AX88772 USB adapter. This file is
//   the same kind of driver for the AX88179 (Nintendo Switch LAN adapter) and the AX88179A.
//   It is compiled to machine code, and build_patch.py copies that code over the old driver.
//   You do NOT need to build anything to use the patcher: the finished result is already
//   in source/ax179_ios*_data.h.
//
// How does it fit into the module?
//   - The old driver sits at 0x13aa0da4..0x13aa1810, the stock stop routine at 0x13aa18cc.
//     The new code is placed there (see block.ld).
//   - Every function marked SEC("e_...") below is an "entry point". It does the same job as one
//     function of the old driver and takes the same arguments, so build_patch.py only has to
//     change the calls of the module to point at it. The comment above each entry point names the
//     old function it replaces (FUN_13aa....).
//   - The three modules have the same layout. Only IOS59 has its syscall veneers 16 bytes further
//     up (IOB_VENEER, set by build_patch.py).
//
// Things to know when reading the code
//   - The CPU is ARM926EJ-S in Thumb mode and big-endian. The USB chip is little-endian, so
//     register values and packet headers are put together byte by byte.
//   - There is no operating system, no libc and no global variables. Everything the driver needs
//     is either passed in (ctx) or already exists in the module (the functions and variables below).
//   - The two adapters are driven in different ways:
//       AX88179   vendor registers (the sequences come from a usbmon capture of the Linux driver)
//       AX88179A  CDC-ECM mode, USB configuration 3, plain ethernet frames (see ecm_init())

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef int            s32;

// ---- functions and variables that already exist in the module -------------------------------

// USB control transfer: (fd, ep0, bmRequestType, bRequest, wValue, wIndex, wLength, buffer)
#define USB_CONTROL ((s32 (*)(u32, u32, u32, u32, u32, u32, u32, void *))0x13aa1ac1)  // FUN_13aa1ac0
// USB interrupt-in transfer: (fd, ep0, buffer, length, endpoint)
#define USB_INTR    ((s32 (*)(u32, u32, void *, u32, u32))0x13aa1b25)                 // FUN_13aa1b24
// USB bulk transfer: (fd, ep0, buffer, length, endpoint)
#define USB_BULK    ((s32 (*)(u32, u32, void *, u32, u32))0x13aa1b7d)                 // FUN_13aa1b7c
// wait for some microseconds
#define DELAY_US    ((void (*)(u32))0x13aa23f5)                                       // FUN_13aa23f4
// cancel the transfers of an endpoint (USBV5 CANCELENDPOINT): (fd, ep0, endpoint)
#define USB_CANCEL  ((s32 (*)(u32, u32, u32))0x13aa1bf9)                              // FUN_13aa1bf8

#ifndef IOB_VENEER
#define IOB_VENEER 0x13aa2a68                              // IOS58 / IOS57. IOS59: 0x13aa2a78
#endif
// The module's packet buffers ("IOB"): push makes room for a header in front of the data,
// pull removes it again. Both are system calls, reached through a small veneer.
#define IOB_PUSH ((u8 *(*)(void *, u32))(IOB_VENEER + 1))  // syscall 0x3b
#define IOB_PULL ((s32 (*)(void *, u32))(IOB_VENEER + 9))  // syscall 0x3c

// variables of the module (the old driver used them too)
#define DEVICE_FD    (*(volatile u32 *)0x13ab0184)         // handle of the USB device
#define EP0_HANDLE   (*(volatile u32 *)0x13ab0188)         // handle of endpoint 0
#define RX_MAX_BYTES (*(volatile u32 *)0x13ab0140)         // size of one bulk-in transfer (4 KB)
#define SCRATCH      ((u8 *)0x13aa8120)                    // buffer that is safe for USB DMA

// ---- the "ctx" structure the module passes to every entry point -----------------------------
// It belongs to the module. These are the words (u32 index) and bytes this driver uses.
#define CTX_FD         0       // device handle
#define CTX_MAC_BYTE   0xc     // BYTE offset (not index!): the 6 bytes of the MAC address
#define CTX_LINK_UP    0xc     // 1 = link is up (here an index again: word 0xc, byte 0x30)
#define CTX_RX_BUFFER  0xd     // buffer for the received data
#define CTX_RX_HEADERS 0xe     // AX88179 receive state: where the frame headers are | count << 16
#define CTX_RX_POS     0xf     // AX88179 receive state: data offset | frame index << 16.
                               // In ECM mode this word holds ECM_FLAG instead.

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

#define RX_CTL_PRO    0x0001       // promiscuous: receive everything
#define RX_CTL_AMALL  0x0002       // receive all multicast frames
#define RX_CTL_BASE   0x0298       // IPE | START | AM | AB (as in the capture)
#define RX_CTL_INIT   0x03aa       // what linux writes during reset

#define MEDIUM_BASE   0x0130       // RECEIVE_EN | TXFLOW | RXFLOW
#define MEDIUM_INIT   0x0133       // + FULL_DUPLEX | GIGAMODE before the link is up
#define MEDIUM_RECEIVE_EN 0x0100

#define PHYPWR_IPRL   0x0020
#define PHYPWR_BZ     0x0010       // bulk-in returns zero length (unblocks the rx thread)

// Bulk-in aggregation {ctrl, timer_l, timer_h, size, ifg}: the module reads RX_MAX_BYTES (4 KB)
// per transfer, so the chip must not gather more than that. 'size' is in KB (linux: 0x16 with
// 24 KB URBs). 2 KB leaves room for one more full frame plus headers.
#define BULKIN_SIZE_KB 2

#define EP_OUT 3                   // bulk out (we send)
#define EP_IN  0x82                // bulk in (we receive)
#define EP_INT 0x81                // interrupt in (link status)
#define HS_MAXPKT_MASK 511         // USB 2.0 high speed: packets are 512 bytes long
#define RXH_ERR_MASK   0xA0000000u // CRC_ERR (bit 29) | DROP_ERR (bit 31)

// Puts a function into its own section, so that block.ld can place it and keep it.
#define SEC(x) __attribute__((section(".text." x), used))

// ---- AX88179A in CDC-ECM mode ---------------------------------------------------------------
// USB configuration 3: raw ethernet frames on bulk 0x82 / 0x03, control interface 0, data
// interface 1 alt 1. The vendor configuration does not come up on the Wii U. The AX88179A is
// recognised by bNumConfigurations >= 3 (the AX88179 has 1).
// CTX_RX_POS holds ECM_FLAG (the AX88179 receive state is not used in ECM mode, rxreset keeps it).
#define ECM_FLAG   0x80000000u
#define ECM_CONFIG 3
#define ECM_DATA_IF 1
#define ECM_MAC_STR 5            // the MAC address is the USB string with this number
#define ECM_RXLEN 1536           // one frame per transfer
#define ECM_FILTER 0x000f        // promiscuous | all multicast | directed | broadcast
#define ECM_SETTLE_US 1000000    // tx frames got lost during the first ~400 ms after the switch
#define IS_ECM(ctx) ((ctx)[CTX_RX_POS] & ECM_FLAG)

// ---- standard USB requests ------------------------------------------------------------------
// tr = bmRequestType | bRequest << 8, il = wIndex | wLength << 16 (fewer stack arguments)
static s32 std_req(u32 fd, u32 tr, u32 val, u32 il)
{
    return USB_CONTROL(fd, EP0_HANDLE, tr & 0xff, tr >> 8, val, il & 0xffff, il >> 16, SCRATCH);
}
#define STD(fd, type, req, val, idx, len) std_req(fd, (type) | (req) << 8, val, (idx) | (len) << 16)

// the answers land in SCRATCH
#define GET_DESCRIPTOR_DEVICE(fd)  STD(fd, 0x80, 6, 0x0100, 0, 18)
#define GET_CONFIGURATION(fd)      STD(fd, 0x80, 8, 0, 0, 1)
#define SET_CONFIGURATION(fd, n)   STD(fd, 0x00, 9, n, 0, 0)
#define SET_INTERFACE(fd, i, alt)  STD(fd, 0x01, 0x0b, alt, i, 0)

static s32 ecm_filter(u32 fd)
{
    return STD(fd, 0x21, 0x43, ECM_FILTER, 0, 0);   // SET_ETHERNET_PACKET_FILTER, control IF 0
}

// Starts the AX88179A: switches to configuration 3 (only if it is not active yet - a repeated
// SET_CONFIGURATION / SET_INTERFACE resets the data toggles), sets the filter and takes the MAC
// address from string 5 (12 hex digits, a missing digit counts as 0, no string at all gives 02:00:..).
// No link detection: ECM reports changes only on the interrupt endpoint and a blocking read
// there would stall the module, so the link counts as up from init to stop.
static s32 ecm_init(u32 *ctx, u32 *li)
{
    u32 fd = ctx[CTX_FD];
    s32 r = GET_CONFIGURATION(fd);
    if (r < 0)
        return r;

    u32 must_switch = SCRATCH[0] != ECM_CONFIG;
    if (must_switch) {
        r = SET_CONFIGURATION(fd, ECM_CONFIG);
        if (r < 0)
            return r;
        r = SET_INTERFACE(fd, ECM_DATA_IF, 1);
        if (r < 0)
            return r;
        DELAY_US(ECM_SETTLE_US);
    }
    r = ecm_filter(fd);
    if (r < 0)
        return r;

    // After a switch, read the three notifications (disconnected, speed, connected) first,
    // otherwise the module's bulk-in never returns. The adapter negotiates its link speed
    // itself, to keep it at 100M the switch / router port has to be limited.
    if (must_switch) {
        for (int i = 0; i < 3; i++) {
            if (USB_INTR(fd, EP0_HANDLE, SCRATCH, 16, EP_INT) < 0)
                break;
        }
    }

    // The MAC address is a string descriptor in UTF-16: 12 hex digits, one every 2 bytes,
    // starting at byte 2 (after length and type). Every 2 digits make one byte of the address.
    u8 *mac = (u8 *)ctx + CTX_MAC_BYTE;
    if (STD(fd, 0x80, 6, 0x0300 | ECM_MAC_STR, 0x0409, 26) < 0)
        SCRATCH[2] = 0;                                 // no string: no digits at all
    // 'end' becomes 1 at the first character that is no hex digit, the digits after it are 0
    for (u32 i = 0, end = 0; i < 12; i++) {
        u32 ch = SCRATCH[2 + 2 * i];
        u32 digit = ch - '0';
        if (digit > 9)
            digit = (ch | 0x20) - 'a' + 10;             // a..f (and A..F)
        if (digit > 15)
            end = 1;
        if (end)
            digit = 0;
        mac[i >> 1] = (i & 1) ? mac[i >> 1] | digit     // odd digit: low half of the byte
                              : digit << 4;             // even digit: high half of the byte
    }
    if (!(mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]))
        mac[0] = 0x02;                                  // no string: a "local" address

    // tell the module what the link looks like
    li[0] = 0x4f;                                       // autoneg, 100/10 full/half
    li[1] = 0x4f;
    LI_SPEED(li) = 100;
    LI_DUPLEX(li) = 1;
    LI_AUTONEG(li) = 1;
    ctx[CTX_LINK_UP] = 1;
    return 0;
}

// reads 4 bytes of a little-endian buffer
static inline __attribute__((always_inline)) u32 le32(const u8 *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
}

// ---- register access (AX88179) --------------------------------------------------------------

// write len (1 or 2, or 5, 6, 8 for the helpers further down) bytes, the value is little-endian
static s32 wr(u32 fd, u32 req, u32 val, u32 idx, u32 len, u32 data)
{
    u8 *s = SCRATCH;
    for (u32 i = 0; i < len; i++)
        s[i] = data >> (8 * i);
    return USB_CONTROL(fd, EP0_HANDLE, 0x40, req, val, idx, len, s);
}

// read len (1 or 2) bytes, returns the value or a negative error
static s32 rd(u32 fd, u32 req, u32 val, u32 idx, u32 len)
{
    u8 *s = SCRATCH;
    s32 r = USB_CONTROL(fd, EP0_HANDLE, 0xc0, req, val, idx, len, s);
    if (r < 0)
        return r;
    return len == 1 ? s[0] : (s[0] | (s[1] << 8));
}
#define MAC_W(fd, reg, len, v) wr(fd, REQ_MAC, reg, len, len, v)
#define PHY_W(fd, reg, v)      wr(fd, REQ_PHY, PHY_ID, reg, 2, v)
#define PHY_R(fd, reg)         rd(fd, REQ_PHY, PHY_ID, reg, 2)

// sets how much the chip gathers into one bulk-in transfer
static s32 bulkin_cfg(u32 fd)
{
    u8 *s = SCRATCH;
    s[0] = 0x07; s[1] = 0x20; s[2] = 0x03; s[3] = BULKIN_SIZE_KB; s[4] = 0xff;
    return USB_CONTROL(fd, EP0_HANDLE, 0x40, REQ_MAC, R_BULKIN_Q, 5, 5, s);
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
// 1000M. Every up runs the full init (PHY reset), which writes it again. Build with GIGABIT=1 to keep
// the chip default.
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
    s32 r = MAC_W(fd, R_PHYPWR, 2, PHYPWR_IPRL);        // clears BZ left by swreset
    if (r < 0)
        return r;
    s32 mode = phy_medium(fd);
    if (mode < 0)
        return mode;
    return MAC_W(fd, R_MEDIUM, 2, (u32)mode);
}

// ===========================================================================================
// Entry points. Each one replaces a function of the old driver (named in the comment).
// ===========================================================================================

// replaces FUN_13aa14e4: remember fd / ep0
SEC("e_handles") void ax179_handles(u32 fd, u32 ep0)
{
    DEVICE_FD = fd;
    EP0_HANDLE = ep0;
}

// replaces FUN_13aa14f8: init(ctx, li) - start the adapter
// li: see "link info" above, derived from the standard MII registers like the original driver does
SEC("e_init") s32 ax179_init(u32 *ctx, u32 *li)
{
    u32 fd = ctx[CTX_FD];
    s32 r;
    ctx[CTX_LINK_UP] = 0;                               // link not configured yet
    ctx[CTX_RX_HEADERS] = 0;
    ctx[CTX_RX_POS] = 0;                                // AX88179 mode

    // The device descriptor: byte 17 is bNumConfigurations. 3 or more -> AX88179A, use CDC-ECM.
    r = GET_DESCRIPTOR_DEVICE(fd);
    if (r < 0)
        return r;
    if (SCRATCH[17] >= ECM_CONFIG) {
        ctx[CTX_RX_POS] = ECM_FLAG;
        return ecm_init(ctx, li);
    }

    // --- AX88179: reset the PHY and clocks
    r = MAC_W(fd, R_PHYPWR, 2, 0);
    if (r < 0)
        return r;
    MAC_W(fd, R_PHYPWR, 2, PHYPWR_IPRL);
    DELAY_US(500000);
    MAC_W(fd, R_CLK_SELECT, 1, 0x03);                   // ACS | BCS
    DELAY_US(200000);

    // --- the MAC address comes from the chip (eFuse), not from the EEPROM
    r = USB_CONTROL(fd, EP0_HANDLE, 0xc0, REQ_MAC, R_NODE_ID, 6, 6, SCRATCH);
    if (r < 0)
        return r;
    for (int i = 0; i < 6; i++)
        ((u8 *)ctx)[CTX_MAC_BYTE + i] = SCRATCH[i];

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

// replaces FUN_13aa18cc (interface down): stop receiving, power down the PHY.
// Only RECEIVE_EN is cleared in MEDIUM, like the stock routine and linux ax88179_stop()
// (writing 0 would also drop GIGAMODE / FULL_DUPLEX). RX_CTL stays on: the module stops the
// rx thread afterwards through ax179_swreset, and BZ only completes the pending bulk-in while
// the bulk-in engine still runs. The next up runs the full init again.
SEC("e_stop") s32 ax179_stop(u32 *ctx)
{
    u32 fd = ctx[CTX_FD];
    ctx[CTX_LINK_UP] = 0;
    if (IS_ECM(ctx))
        return 0;                                       // data interface stays active (toggles)
    s32 m = rd(fd, REQ_MAC, R_MEDIUM, 2, 2);
    if (m < 0)
        return -0x15;
    if (MAC_W(fd, R_MEDIUM, 2, (u32)m & ~MEDIUM_RECEIVE_EN) < 0)
        return -0x15;
    if (MAC_W(fd, R_PHYPWR, 2, 0) < 0)
        return -0x15;
    return 0;
}

// replaces the call FUN_13aa1878 -> FUN_13aa0f1c(fd, 0x3c): make a pending bulk-in return so
// the rx thread can be stopped (link_reset / init clear BZ again).
// IPRL | BZ takes the PHY out of reset. After ax179_stop (RECEIVE_EN cleared, a plain link loss
// leaves it set) the PHY goes back into reset once the zero length transfer had time to
// complete, otherwise the port comes back up after down. FUN_13aa1878 waits another 120 ms.
SEC("e_swreset") s32 ax179_swreset(u32 fd, u32 v)
{
    (void)v;
    // ECM has no BZ bit: cancel the pending bulk-in like the module's close path (FUN_13aa1898).
    // Always 0, on an error FUN_13aa0230 would skip the join and leave the rx thread running.
    // If the cancel does nothing, the join returns with the next frame (promiscuous filter).
    if (GET_CONFIGURATION(fd) >= 0 && SCRATCH[0] == ECM_CONFIG) {
        USB_CANCEL(fd, EP0_HANDLE, EP_IN);
        return 0;
    }
    s32 m = rd(fd, REQ_MAC, R_MEDIUM, 2, 2);
    s32 r = MAC_W(fd, R_PHYPWR, 2, PHYPWR_IPRL | PHYPWR_BZ);
    if (r >= 0 && m >= 0 && !(m & MEDIUM_RECEIVE_EN)) {
        DELAY_US(50000);                                // pending bulk-in completes with 0 bytes
        r = MAC_W(fd, R_PHYPWR, 2, 0);                  // PHY back in reset, port stays down
    }
    return r;
}

// replaces FUN_13aa1068: rxmode(ctx, filter) - which frames are received
// filter: [0] flags (bit 3 promisc, bit 4 allmulti), [1] count, then count * 6 byte addresses
SEC("e_rxmode") s32 ax179_rxmode(u32 *ctx, s32 *f)
{
    u32 fd = ctx[CTX_FD];
    if (IS_ECM(ctx))
        return ecm_filter(fd) < 0 ? -0x15 : 0;

    u32 rxctl = RX_CTL_BASE;
    if (f[0] & 0x08)
        rxctl |= RX_CTL_PRO;
    if (f[0] & 0x10)
        rxctl |= RX_CTL_AMALL;

    // The chip has a 64 bit table (8 bytes) for multicast addresses. The CRC of an address
    // chooses one bit of it, the same way as the linux function ether_crc().
    u8 table[8];
    for (int i = 0; i < 8; i++)
        table[i] = 0;
    const u8 *addr = (const u8 *)(f + 2);
    for (u32 n = (u32)f[1]; n; n--, addr += 6) {
        u32 crc = 0xffffffffu;
        for (int i = 0; i < 6; i++) {
            u32 d = addr[i];
            for (int k = 0; k < 8; k++, d >>= 1) {
                u32 c = (crc >> 31) ^ (d & 1);
                crc <<= 1;
                if (c)
                    crc = (crc ^ 0x04c11db7u) | 1;
            }
        }
        crc >>= 26;                                      // the upper 6 bits: bit number 0..63
        table[crc >> 3] |= 1 << (crc & 7);
    }
    for (int i = 0; i < 8; i++)
        SCRATCH[i] = table[i];
    if (USB_CONTROL(fd, EP0_HANDLE, 0x40, REQ_MAC, R_MULFLTARY, 8, 8, SCRATCH) < 0)
        return -0x15;
    if (MAC_W(fd, R_RX_CTL, 2, rxctl) < 0)
        return -0x15;
    return 0;
}

// replaces FUN_13aa1184: link(ctx) - is the link up? 0 = up, -0x18 = down
SEC("e_link") s32 ax179_link(u32 *ctx)
{
    if (IS_ECM(ctx))
        return ctx[CTX_LINK_UP] ? 0 : -0x18;

    // The chip reports the link on the interrupt endpoint. Byte 2, bit 0 is the link bit.
    s32 r = USB_INTR(ctx[CTX_FD], EP0_HANDLE, SCRATCH, 8, EP_INT);
    if (r < 0)
        return r;
    if (!(SCRATCH[2] & 1)) {
        ctx[CTX_LINK_UP] = 0;
        return -0x18;
    }
    if (!ctx[CTX_LINK_UP]) {                            // the link was down before
        r = link_reset(ctx[CTX_FD]);
        if (r < 0)
            return r;
        ctx[CTX_LINK_UP] = 1;
    }
    return 0;
}

// replaces FUN_13aa1328: reset the receive state
SEC("e_rxreset") s32 ax179_rxreset(u32 *ctx)
{
    ctx[CTX_RX_HEADERS] = 0;
    ctx[CTX_RX_POS] &= ECM_FLAG;                        // keep the ECM flag
    return 0;
}

// replaces FUN_13aa0fec (ioctl get MAC)
SEC("e_getmac") s32 ax179_getmac(u32 *ctx, u8 *out)
{
    if (IS_ECM(ctx)) {
        for (int i = 0; i < 6; i++)
            out[i] = ((u8 *)ctx)[CTX_MAC_BYTE + i];
        return 0;
    }
    s32 r = USB_CONTROL(ctx[CTX_FD], EP0_HANDLE, 0xc0, REQ_MAC, R_NODE_ID, 6, 6, SCRATCH);
    for (int i = 0; i < 6; i++)
        out[i] = SCRATCH[i];
    return r;
}

// replaces FUN_13aa102c (ioctl set MAC)
SEC("e_setmac") s32 ax179_setmac(u32 *ctx, const u8 *mac)
{
    // ECM: ignored, the stack keeps the adapter's address (the AX88179 re-reads NODE_ID on init too)
    if (IS_ECM(ctx))
        return 0;
    for (int i = 0; i < 6; i++)
        SCRATCH[i] = mac[i];
    return USB_CONTROL(ctx[CTX_FD], EP0_HANDLE, 0x40, REQ_MAC, R_NODE_ID, 6, 6, SCRATCH);
}

// replaces FUN_13aa11cc: tx(ctx, iob) - send one frame
// The AX88179 wants an 8 byte header in front of the frame: length (LE32) and flags.
SEC("e_tx") s32 ax179_tx(u32 *ctx, u8 *iob)
{
    // iob: +0x10 pointer to the data, +0x16 length of the data (u16)
    if (IS_ECM(ctx)) {                                  // a raw frame, no header
        u32 len = *(u16 *)(iob + 0x16);
        if (!(len & HS_MAXPKT_MASK))
            len++;                                      // one pad byte instead of a ZLP (as usbnet)
        s32 r = USB_BULK(ctx[CTX_FD], EP0_HANDLE, *(u8 **)(iob + 0x10), len, EP_OUT);
        return r < 0 ? r : 0;
    }

    u8 *h = IOB_PUSH(iob, 8);                           // make room for the header
    if (!h)
        return -0x16;
    u32 total = *(u16 *)(iob + 0x16);                   // length including the header
    u32 plen  = total - 8;
    u8  pad   = (total & HS_MAXPKT_MASK) ? 0 : 0x80;    // header word 2 = 0x80008000: add a pad byte
    h[0] = plen; h[1] = plen >> 8; h[2] = 0; h[3] = 0;
    h[4] = 0;    h[5] = pad;       h[6] = 0; h[7] = pad;
    s32 r = USB_BULK(ctx[CTX_FD], EP0_HANDLE, h, total, EP_OUT);
    if (r < 0)
        return r;
    return IOB_PULL(iob, 8) ? 0 : -0x16;               // take the header off again
}

// replaces FUN_13aa1234: rx(ctx, u8 **frame, u16 *len) - receive one frame
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
        if (!(buf && RX_MAX_BYTES))
            return -0x16;
        s32 n = USB_BULK(ctx[CTX_FD], EP0_HANDLE, buf, ECM_RXLEN, EP_IN);
        if (n < 14) {                                   // nothing, or shorter than an ethernet header
            *out_len = 0;
            return n < 0 ? n : 0;
        }
        *out_ptr = buf;
        *out_len = (u16)n;
        return 0;
    }

    for (;;) {
        u32 a = ctx[CTX_RX_HEADERS], b = ctx[CTX_RX_POS];
        u32 headers_at = a & 0xffff, offset = b & 0xffff, index = b >> 16;

        if (index >= (a >> 16)) {
            // All frames of the last transfer are used up: read the next transfer.
            u32 max = RX_MAX_BYTES;
            ctx[CTX_RX_HEADERS] = 0;
            ctx[CTX_RX_POS] = 0;
            if (!(buf && max))
                return -0x16;
            s32 n = USB_BULK(ctx[CTX_FD], EP0_HANDLE, buf, max, EP_IN);
            if (n < 8) {
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
        return 0;
    }
}
