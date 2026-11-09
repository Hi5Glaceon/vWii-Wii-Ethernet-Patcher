// 772D patches - PID 0x1790, indirect register access, 8-byte packet framing

#include <stdio.h>
#include <string.h>
#include <gccore.h>
#include "patch_common.h"
#include "patch_772d.h"
#include "sha1.h"

// code injection goes in error string region (8.9KB in LOAD RX segment)

static const u8 pat_ehc[] = {
    0x2F,0x64,0x65,0x76,0x2F,0x75,0x73,0x62,
    0x2F,0x65,0x68,0x63,0x2F,0x30,0x62,0x39,
    0x35,0x2F,0x37,0x37,0x32
};

static const u8 pat_mov_pid[] = { 0xE3, 0xA0, 0x3C, 0x77 };

static const u8 pat_rxctrl_b[] = { 0xE3,0xA0,0x1F,0x86, 0xEA,0xFF,0xFF,0x8C };
static const u8 pat_rxctrl_b_old[] = { 0xE3,0xA0,0x1F,0x46, 0xEA,0xFF,0xFF,0x8C };
static const u8 pat_rxctrl_b_new[] = { 0xE3,0xA0,0x10,0x18, 0xEA,0xFF,0xFF,0x8C };
static const u8 pat_rxctrl_c[] = { 0xE3,0xA0,0x1F,0xC6 };
static const u8 pat_rxctrl_c_old[] = { 0xE3,0xA0,0x1F,0x46 };
static const u8 pat_rxctrl_c_new[] = { 0xE3,0xA0,0x10,0x18 };

static const u8 pat_swrst_init[] = { 0xE5,0x9A,0x00,0x00, 0xE3,0xA0,0x10,0x44 };
static const u8 pat_swrst_init_new[] = { 0xE5,0x9A,0x00,0x00, 0xE3,0xA0,0x10,0xC4 };
static const u8 pat_swrst_down[] = { 0xE5,0x94,0x00,0x00, 0xE3,0xA0,0x10,0x4C };
static const u8 pat_swrst_down_new[] = { 0xE5,0x94,0x00,0x00, 0xE3,0xA0,0x10,0xCC };

static const u8 pat_vidpid_func[] = {
    0xE3,0xA0,0x20,0x00, 0xE1,0x52,0x00,0x01,
    0xE5,0x2D,0xE0,0x04, 0xE3,0xE0,0xE0,0x05,
    0x2A,0x00,0x00,0x09
};
static const u8 pat_vidpid_func_new[] = {
    0xE3,0xA0,0x20,0x00, 0xE1,0x52,0x00,0x01,
    0xE5,0x2D,0xE0,0x04, 0xE3,0xE0,0xE0,0x05,
    0x2A,0x00,0x00,0x08, 0xE5,0x9F,0xC0,0x30,
    0xE5,0x90,0x30,0x04, 0xE3,0xC3,0x30,0x0F,
    0xE1,0x53,0x00,0x0C, 0xE2,0x82,0x20,0x01,
    0xE2,0x80,0x00,0x08, 0x0A,0x00,0x00,0x04,
    0xE1,0x52,0x00,0x01, 0x3A,0xFF,0xFF,0xF7,
    0xE1,0xA0,0x00,0x0E, 0xE4,0x9D,0xE0,0x04,
    0xE1,0x2F,0xFF,0x1E, 0xE3,0xA0,0xE0,0x00,
    0xEA,0xFF,0xFF,0xFA, 0x0B,0x95,0x77,0x20
};

static const u8 pat_ios58_vidpid[] = { 0x0B,0x95,0x77,0x20, 0x00,0xFF,0xFF,0xFF };

static const u8 pat_ios58_rxc_318[] = { 0x21,0xC6, 0x00,0x89 };
static const u8 pat_ios58_rxc_218[] = { 0x21,0x86, 0x00,0x89 };
static const u8 pat_ios58_rxc_118[] = { 0x21,0x46, 0x00,0x89 };
static const u8 pat_ios58_rxc_018[] = { 0x21,0x06, 0x00,0x89 };

static const u8 pat_ios58_swrst[] = { 0x21,0x4C };
static const u8 pat_ios58_swrst_new[] = { 0x21,0xCC };

// file_offset = vaddr - 0x13AA0000 + 0x128
#define ARM32_ELF_BASE     0x0128
#define ARM32_VA_BASE      0x13AA0000
#define ARM32_FO_AXINIT       0x0FA4
#define ARM32_FO_RECVPACKET   0x0B7C
#define ARM32_FO_VENDORCMD    0x03D8
#define ARM32_FO_VIDPID       0x1FD0
#define ARM32_FO_XMITPACKET   0x0AA4
#define ARM32_FO_IOCTLV_WRAP  0x5248   // USB vendor cmd funnel
static u32 arm32_branch(u32 from_va, u32 to_va)
{
    s32 offset = ((s32)(to_va - from_va - 8)) >> 2;
    return 0xEA000000 | (offset & 0x00FFFFFF);
}

static void write_be32(u8 *buf, u32 val)
{
    buf[0] = (val >> 24) & 0xFF;
    buf[1] = (val >> 16) & 0xFF;
    buf[2] = (val >> 8) & 0xFF;
    buf[3] = val & 0xFF;
}

static const u8 pat_772d_ehc_path[] = {
    0x2F,0x64,0x65,0x76,0x2F,0x75,0x73,0x62,
    0x2F,0x65,0x68,0x63,0x2F,0x30,0x62,0x39,
    0x35,0x2F,0x31,0x37,0x39,0x30
};
static const u8 pat_772d_oh0_path[] = {
    0x2F,0x64,0x65,0x76,0x2F,0x75,0x73,0x62,
    0x2F,0x6F,0x68,0x30,0x2F,0x30,0x62,0x39,
    0x35,0x2F,0x31,0x37,0x39,0x30
};

// vidpid scanner rewrite to match 0x0B951790
static const u8 pat_772d_vidpid_func[] = {
    0xE3,0xA0,0x20,0x00,
    0xE1,0x52,0x00,0x01,
    0xE5,0x2D,0xE0,0x04,
    0xE3,0xE0,0xE0,0x05,
    0x2A,0x00,0x00,0x07,
    0xE5,0x9F,0xC0,0x30,
    0xE5,0x90,0x30,0x04,
    0xE1,0x53,0x00,0x0C,
    0x0A,0x00,0x00,0x06,
    0xE2,0x82,0x20,0x01,
    0xE2,0x80,0x00,0x08,
    0xE1,0x52,0x00,0x01,
    0x3A,0xFF,0xFF,0xF8,
    0xE1,0xA0,0x00,0x0E,
    0xE4,0x9D,0xE0,0x04,
    0xE1,0x2F,0xFF,0x1E,
    0xE3,0xA0,0xE0,0x00,
    0xEA,0xFF,0xFF,0xFA,
    0x00,0x00,0x00,0x00,
    0x0B,0x95,0x17,0x90,  // .word 0x0B951790
    0x00,0x00,0x00,0x00,
};

static const u8 pat_772d_mov_pid_hi[] = { 0xE3, 0xA0, 0x3C, 0x17 };
static const u8 pat_772d_add_pid_lo = 0x90;

// 772A -> 772D command translation table
// access_type: 0x01=MAC, 0x02=PHY, 0x04=EEPROM, 0x00=nop
typedef struct {
    u8 cmd_772a;
    u8 access_type;
    u8 reg_addr;     // wValue for MAC, phy_id for PHY
    u8 data_len;
} cmd_xlat_t;

static const cmd_xlat_t cmd_map_772d[] = {
    { 0x06, 0x00, 0x00, 0 },  // SET_SW_MII -> nop
    { 0x07, 0x02, 0x03, 2 },  // READ_MII_REG -> ACCESS_PHY
    { 0x08, 0x02, 0x03, 2 },  // WRITE_MII_REG -> ACCESS_PHY
    { 0x09, 0x00, 0x00, 0 },  // STATMNGSTS_REG -> nop
    { 0x0A, 0x00, 0x00, 0 },  // SET_HW_MII -> nop
    { 0x0B, 0x04, 0x00, 2 },  // READ_EEPROM -> ACCESS_EEPROM
    { 0x0C, 0x04, 0x00, 2 },  // WRITE_EEPROM -> ACCESS_EEPROM
    { 0x0D, 0x04, 0x00, 0 },  // WRITE_ENABLE -> ACCESS_EEPROM
    { 0x0E, 0x04, 0x00, 0 },  // WRITE_DISABLE -> ACCESS_EEPROM
    { 0x0F, 0x01, 0x0B, 2 },  // READ_RX_CTL -> ACCESS_MAC reg 0x0B
    { 0x10, 0x01, 0x0B, 2 },  // WRITE_RX_CTL -> ACCESS_MAC reg 0x0B
    { 0x13, 0x01, 0x10, 6 },  // READ_NODE_ID -> ACCESS_MAC reg 0x10
    { 0x14, 0x01, 0x10, 6 },  // WRITE_NODE_ID -> ACCESS_MAC reg 0x10
    { 0x16, 0x01, 0x16, 8 },  // WRITE_MULTI_FILTER -> ACCESS_MAC reg 0x16
    { 0x1A, 0x01, 0x22, 2 },  // READ_MEDIUM -> ACCESS_MAC reg 0x22
    { 0x1B, 0x01, 0x22, 2 },  // WRITE_MEDIUM -> ACCESS_MAC reg 0x22
    { 0x1C, 0x01, 0x24, 1 },  // READ_MONITOR_MODE -> ACCESS_MAC reg 0x24
    { 0x1D, 0x01, 0x24, 1 },  // WRITE_MONITOR_MODE -> ACCESS_MAC reg 0x24
    { 0x1F, 0x01, 0x25, 1 },  // WRITE_GPIOS -> ACCESS_MAC reg 0x25
    { 0x20, 0x01, 0x26, 2 },  // SW_RESET -> ACCESS_MAC 0x26
    { 0x21, 0x01, 0x26, 2 },  // SW_PHY_STATUS -> ACCESS_MAC 0x26
    { 0x22, 0x00, 0x00, 0 },  // SW_PHY_SELECT -> nop
};
#define CMD_MAP_772D_COUNT (sizeof(cmd_map_772d) / sizeof(cmd_map_772d[0]))

// 772D init register values
#define AX772D_PHYPWR_RSTCTL_CLR  0x0000
#define AX772D_PHYPWR_RSTCTL_IPRL 0x0020
#define AX772D_CLK_SELECT         0x03     // ACS | BCS
#define AX772D_RXCOE_CTL          0x00
#define AX772D_TXCOE_CTL          0x00
#define AX772D_RX_CTL             0x0088   // START | AB
#define AX772D_MEDIUM_MODE        0x0332   // RE|TXFLOW|RXFLOW|FDX|100M
#define AX772D_MONITOR_MODE       0x00
#define AX772D_PHY_ID             0x03

s32 patch_772d_arm32(IOS *ios)
{
    tmd *t = (tmd *)SIGNATURE_PAYLOAD(ios->tmd);
    tmd_content *cr = TMD_CONTENTS(t);
    u8 hash[20];
    int applied = 0, existing = 0;
    s32 r;

    s32 index = find_eth_module(ios);
    if (index < 0) return -1;
    printf("772D: eth module is content #%d\n", index);

    u8 *buf = ios->decrypted_buffer[index];
    u32 size = (u32)cr[index].size;

    // patch device path strings to "1790"
    {
        s32 off = find_pattern(buf, size, pat_ehc, 18);
        if (off < 0) {
            // try partial match in case 772B patch already changed the suffix
            u8 pat_ehc_b[] = {
                0x2F,0x64,0x65,0x76,0x2F,0x75,0x73,0x62,
                0x2F,0x65,0x68,0x63,0x2F,0x30,0x62,0x39,
                0x35,0x2F
            };
            off = find_pattern(buf, size, pat_ehc_b, sizeof(pat_ehc_b));
        }
        if (off >= 0 && (u32)(off + 22) <= size) {
            buf[off + 18] = '1';
            buf[off + 19] = '7';
            buf[off + 20] = '9';
            buf[off + 21] = '0';
            printf("  ehc path -> 1790\n");
            applied++;
        } else {
            if (find_pattern(buf, size, pat_772d_ehc_path, sizeof(pat_772d_ehc_path)) >= 0) {
                printf("  ehc path: already 1790\n"); existing++;
            } else {
                printf("  ehc path: not found\n"); return -1;
            }
        }

        u8 pat_oh0_base[] = {
            0x2F,0x64,0x65,0x76,0x2F,0x75,0x73,0x62,
            0x2F,0x6F,0x68,0x30,0x2F,0x30,0x62,0x39,
            0x35,0x2F
        };
        off = find_pattern(buf, size, pat_oh0_base, sizeof(pat_oh0_base));
        if (off >= 0 && (u32)(off + 22) <= size) {
            buf[off + 18] = '1';
            buf[off + 19] = '7';
            buf[off + 20] = '9';
            buf[off + 21] = '0';
            printf("  oh0 path -> 1790\n");
            applied++;
        } else {
            if (find_pattern(buf, size, pat_772d_oh0_path, sizeof(pat_772d_oh0_path)) >= 0) {
                printf("  oh0 path: already 1790\n"); existing++;
            } else {
                printf("  oh0 path: not found\n"); return -1;
            }
        }
    }

    // PID 0x7720 -> 0x1790
    {
        s32 off = find_pattern(buf, size, pat_mov_pid, sizeof(pat_mov_pid));
        if (off < 0) {
            if (find_pattern(buf, size, pat_772d_mov_pid_hi, sizeof(pat_772d_mov_pid_hi)) >= 0) {
                printf("  pid imm: already 772D\n"); existing++;
            } else {
                printf("  pid imm: not found\n"); return -1;
            }
        } else {
            buf[off + 3] = 0x17;
            printf("  pid hi @ 0x%04X: 0x77 -> 0x17\n", off + 3);
            u8 *add = buf + off + 16;
            if (add[0] == 0xE2 && add[1] == 0x83 && add[2] == 0x30) {
                printf("  pid lo @ 0x%04X: 0x%02X -> 0x90\n", (s32)(add - buf) + 3, add[3]);
                add[3] = pat_772d_add_pid_lo;
                applied++;
            } else {
                printf("  pid ADD: unexpected\n"); return -1;
            }
        }
    }

    // vidpid scanner -> match 0x0B951790
    {
        s32 off = find_pattern(buf, size, pat_vidpid_func, sizeof(pat_vidpid_func));
        if (off < 0)
            off = find_pattern(buf, size, pat_vidpid_func_new, 20);
        if (off >= 0 && (u32)(off + 80) <= size) {
            printf("  vidpid scanner @ 0x%04X -> 772D\n", off);
            memcpy(buf + off, pat_772d_vidpid_func, 80);
            applied++;
        } else if (find_pattern(buf, size, pat_772d_vidpid_func, 20) >= 0) {
            printf("  vidpid scanner: already 772D\n"); existing++;
        } else {
            printf("  vidpid scanner: not found\n"); return -1;
        }
    }

    // rxctrl - force to 0x0088 (same as 772B, clear header mode bits)
    r = patch_block_compat(buf, size, pat_rxctrl_b, pat_rxctrl_b_old, pat_rxctrl_b_new, sizeof(pat_rxctrl_b), "rxctrl B");
    if (r < 0) return -2;
    if (r == 0) applied++; else existing++;

    r = patch_block_compat(buf, size, pat_rxctrl_c, pat_rxctrl_c_old, pat_rxctrl_c_new, sizeof(pat_rxctrl_c), "rxctrl C");
    if (r < 0) return -2;
    if (r == 0) applied++; else existing++;

    // swrst - IPOSC bit differs on 772D but these patches are harmless
    // and the 772D init writes the correct values anyway
    r = patch_block(buf, size, pat_swrst_init, pat_swrst_init_new, sizeof(pat_swrst_init), "swrst init");
    if (r < 0) return -1;
    if (r == 0) applied++; else existing++;

    r = patch_block(buf, size, pat_swrst_down, pat_swrst_down_new, sizeof(pat_swrst_down), "swrst down");
    if (r < 0) return -1;
    if (r == 0) applied++; else existing++;

    // code cave in error string region, already in a mapped LOAD RX segment
    {
        #define CAVE_FILE_START 0x53C0
        #define CAVE_FILE_END   0x76A4
        #define CAVE_SIZE       (CAVE_FILE_END - CAVE_FILE_START)

        if (size <= CAVE_FILE_END) {
            printf("  error string region: content too small\n");
        } else {
            printf("  code cave: error string region (%u bytes)\n", CAVE_SIZE);
            u32 c = CAVE_FILE_START;

            // cmd translation shim - resolve real IOS_Ioctlv address from trampoline
            u32 wrap_fo = ARM32_FO_IOCTLV_WRAP;
            u32 tramp_offset_word = (buf[wrap_fo+12] << 24) | (buf[wrap_fo+13] << 16) |
                                    (buf[wrap_fo+14] << 8) | buf[wrap_fo+15];
            u32 ioctlv_target_va = tramp_offset_word + 0x13AA512C;
            printf("  IOS_Ioctlv target: 0x%08X\n", ioctlv_target_va);

            u32 shim_start = c;
            u32 shim_va = ARM32_VA_BASE + c - ARM32_ELF_BASE;

            write_be32(buf + c, 0xE92D4070); c += 4;

            write_be32(buf + c, 0xE1A04002); c += 4;

            write_be32(buf + c, 0xE1A05003); c += 4;

            u32 adr_r6_pc = c;
            write_be32(buf + c, 0xE28F6000); c += 4; // ADR R6,table (placeholder)

            u32 loop_fo = c;
            write_be32(buf + c, 0xE5D6C000); c += 4;

            write_be32(buf + c, 0xE35C00FF); c += 4;

            u32 beq_nf = c;
            write_be32(buf + c, 0x0A000000); c += 4;

            write_be32(buf + c, 0xE15C0004); c += 4;

            u32 beq_found = c;
            write_be32(buf + c, 0x0A000000); c += 4;

            write_be32(buf + c, 0xE2866004); c += 4;

            write_be32(buf + c, arm32_branch(
                ARM32_VA_BASE + c - ARM32_ELF_BASE,
                ARM32_VA_BASE + loop_fo - ARM32_ELF_BASE)); c += 4;

                    u32 found_fo = c;
            write_be32(buf + beq_found, 0x0A000000 | ((((s32)((ARM32_VA_BASE+found_fo-ARM32_ELF_BASE)-(ARM32_VA_BASE+beq_found-ARM32_ELF_BASE)-8))>>2)&0xFFFFFF));

            write_be32(buf + c, 0xE5D6C001); c += 4;

            write_be32(buf + c, 0xE35C0000); c += 4;

            u32 beq_rz = c;
            write_be32(buf + c, 0x0A000000); c += 4;

            write_be32(buf + c, 0xE1A0200C); c += 4;

            write_be32(buf + c, 0xE35C0002); c += 4;

            u32 beq_phy = c;
            write_be32(buf + c, 0x0A000000); c += 4;

            write_be32(buf + c, 0xE35C0004); c += 4;

            u32 beq_eep = c;
            write_be32(buf + c, 0x0A000000); c += 4;
            // ACCESS_MAC path
            write_be32(buf + c, 0xE5D63002); c += 4;

            u32 b_call = c;
            write_be32(buf + c, 0xEA000000); c += 4;

            // eeprom path - keep original wValue as address
            u32 eep_fo = c;
            write_be32(buf + beq_eep, 0x0A000000 | ((((s32)((ARM32_VA_BASE+eep_fo-ARM32_ELF_BASE)-(ARM32_VA_BASE+beq_eep-ARM32_ELF_BASE)-8))>>2)&0xFFFFFF));

            write_be32(buf + c, 0xE1A03005); c += 4;

            u32 b_eep_call = c;
            write_be32(buf + c, 0xEA000000); c += 4;

            // PHY path - stash old wValue into wIndex (SP+16, pushed 4 regs)
            u32 phy_fo = c;
            write_be32(buf + beq_phy, 0x0A000000 | ((((s32)((ARM32_VA_BASE+phy_fo-ARM32_ELF_BASE)-(ARM32_VA_BASE+beq_phy-ARM32_ELF_BASE)-8))>>2)&0xFFFFFF));
            write_be32(buf + c, 0xE58D5010); c += 4;

            write_be32(buf + c, 0xE5D63002); c += 4;

            u32 call_fo = c;
            write_be32(buf + b_call, 0xEA000000 | ((((s32)((ARM32_VA_BASE+call_fo-ARM32_ELF_BASE)-(ARM32_VA_BASE+b_call-ARM32_ELF_BASE)-8))>>2)&0xFFFFFF));
            write_be32(buf + b_eep_call, 0xEA000000 | ((((s32)((ARM32_VA_BASE+call_fo-ARM32_ELF_BASE)-(ARM32_VA_BASE+b_eep_call-ARM32_ELF_BASE)-8))>>2)&0xFFFFFF));

            write_be32(buf + c, 0xE8BD4070); c += 4;

            write_be32(buf + c, arm32_branch(
                ARM32_VA_BASE + c - ARM32_ELF_BASE, ioctlv_target_va)); c += 4;

            u32 rz_fo = c;
            write_be32(buf + beq_rz, 0x0A000000 | ((((s32)((ARM32_VA_BASE+rz_fo-ARM32_ELF_BASE)-(ARM32_VA_BASE+beq_rz-ARM32_ELF_BASE)-8))>>2)&0xFFFFFF));

            write_be32(buf + c, 0xE3A00000); c += 4;

            write_be32(buf + c, 0xE8BD8070); c += 4;

            // not found - pass through to real IOS_Ioctlv
            u32 nf_fo = c;
            write_be32(buf + beq_nf, 0x0A000000 | ((((s32)((ARM32_VA_BASE+nf_fo-ARM32_ELF_BASE)-(ARM32_VA_BASE+beq_nf-ARM32_ELF_BASE)-8))>>2)&0xFFFFFF));

            write_be32(buf + c, 0xE1A02004); c += 4;

            write_be32(buf + c, 0xE1A03005); c += 4;

            write_be32(buf + c, 0xE8BD4070); c += 4;

            write_be32(buf + c, arm32_branch(
                ARM32_VA_BASE + c - ARM32_ELF_BASE, ioctlv_target_va)); c += 4;

            // translation table, fix up ADR offset
            u32 table_fo = c;
            {
                u32 pc_at_adr = ARM32_VA_BASE + adr_r6_pc - ARM32_ELF_BASE + 8;
                u32 table_va = ARM32_VA_BASE + table_fo - ARM32_ELF_BASE;
                u32 adr_offset = table_va - pc_at_adr;
                write_be32(buf + adr_r6_pc, 0xE28F6000 | (adr_offset & 0xFFF));
            }
            for (int i = 0; i < (int)CMD_MAP_772D_COUNT; i++) {
                buf[c++] = cmd_map_772d[i].cmd_772a;
                buf[c++] = cmd_map_772d[i].access_type;
                buf[c++] = cmd_map_772d[i].reg_addr;
                buf[c++] = cmd_map_772d[i].data_len;
            }
            buf[c++] = 0xFF; buf[c++] = 0; buf[c++] = 0; buf[c++] = 0;

            while (c & 3) buf[c++] = 0;

            write_be32(buf + wrap_fo, arm32_branch(
                ARM32_VA_BASE + wrap_fo - ARM32_ELF_BASE, shim_va));
            printf("  cmd xlat shim: %u bytes, patched 0x13AA5120\n", c - shim_start);
            applied++;

            // TX header builder - R12=hdr ptr, R4=IOB ptr, writes 8-byte 772D TX hdr
            u32 tx_fo = c;
            u32 tx_va = ARM32_VA_BASE + tx_fo - ARM32_ELF_BASE;
            // get payload len, compute checksum + pad bit, build header word
            write_be32(buf + c, 0xE1D421B6); c += 4;
            write_be32(buf + c, 0xE2422008); c += 4;
            write_be32(buf + c, 0xE1A02802); c += 4;
            write_be32(buf + c, 0xE1A02822); c += 4;
            write_be32(buf + c, 0xE1A03422); c += 4;
            write_be32(buf + c, 0xE0833002); c += 4;
            write_be32(buf + c, 0xE203307F); c += 4;
            write_be32(buf + c, 0xE1820A83); c += 4;
            write_be32(buf + c, 0xE3120001); c += 4;
            write_be32(buf + c, 0x13800201); c += 4;
            // byte-swap to LE and store
            write_be32(buf + c, 0xE0203860); c += 4;
            write_be32(buf + c, 0xE3C338FF); c += 4;
            write_be32(buf + c, 0xE1A00460); c += 4;
            write_be32(buf + c, 0xE0200423); c += 4;
            write_be32(buf + c, 0xE58C0000); c += 4;
            write_be32(buf + c, 0xE3A00000); c += 4;
            write_be32(buf + c, 0xE58C0004); c += 4;
            write_be32(buf + c, 0xE12FFF1E); c += 4;
            printf("  TX hdr builder: %u bytes\n", c - tx_fo);

            // patch PushIob header size 4->8
            if (buf[0x0AB4]==0xE3 && buf[0x0AB5]==0xA0 && buf[0x0AB6]==0x10 && buf[0x0AB7]==0x04) {
                buf[0x0AB7] = 0x08;
                applied++;
            }
            // replace TX header construction with BL to our builder + NOPs
            {
                u32 p = 0x0AE0;
                u32 pva = ARM32_VA_BASE + p - ARM32_ELF_BASE;
                write_be32(buf + p, 0xEB000000 | ((((s32)(tx_va-pva-8))>>2)&0xFFFFFF));
                for (int n = 1; n < 10; n++) write_be32(buf+p+n*4, 0xE1A00000);
                applied++;
            }
            printf("  patched axXmitPacket\n");

            // RX parser - replaces axRecvPacket, handles 772D multi-packet trailer
            // ctx: +0x00=handle, +0x34=rxBuf, +0x38=rxSizeLeft,
            //      +0x3C=pkt_index, +0x40=data_offset
            u32 rx_fo = c;
            u32 rx_va = ARM32_VA_BASE + rx_fo - ARM32_ELF_BASE;
            // prologue, save args
            write_be32(buf+c,0xE92D47F0); c+=4;
            write_be32(buf+c,0xE1A04000); c+=4;
            write_be32(buf+c,0xE1A0A001); c+=4;
            write_be32(buf+c,0xE1A08002); c+=4;
            write_be32(buf+c,0xE5947034); c+=4;

            // load MFB size from literal pool
            u32 rx_ldr_mfb = c;
            write_be32(buf+c,0xE59F5000); c+=4;
            write_be32(buf+c,0xE5955000); c+=4;
            write_be32(buf+c,0xE5946038); c+=4;
            write_be32(buf+c,0xE3560000); c+=4;
            u32 rx_ble = c;
            write_be32(buf+c,0xDA000000); c+=4;

            // have data - load pkt_index, read trailer at rxBuf+rxSizeLeft-8
            write_be32(buf+c,0xE594903C); c+=4;
            write_be32(buf+c,0xE0870006); c+=4;
            write_be32(buf+c,0xE2400008); c+=4;
            // byte-swap trailer word 0 LE->BE (ARMv5 compatible)
            write_be32(buf+c,0xE5901000); c+=4;
            write_be32(buf+c,0xE0213861); c+=4;
            write_be32(buf+c,0xE3C338FF); c+=4;
            write_be32(buf+c,0xE1A01461); c+=4;
            write_be32(buf+c,0xE0211423); c+=4;
            // R1 = pkt_count[12:0] | hdr_offset[31:13]
            write_be32(buf+c,0xE1A02981); c+=4;
            write_be32(buf+c,0xE1A029A2); c+=4;
            write_be32(buf+c,0xE1590002); c+=4;
            u32 rx_bge = c;
            write_be32(buf+c,0xAA000000); c+=4;
            // pkt_hdr = rxBuf + hdr_offset + pkt_index*8
            write_be32(buf+c,0xE1A036A1); c+=4;
            write_be32(buf+c,0xE0830189); c+=4;
            write_be32(buf+c,0xE0870000); c+=4;
            // byte-swap pkt_hdr word 0
            write_be32(buf+c,0xE5901000); c+=4;
            write_be32(buf+c,0xE0213861); c+=4;
            write_be32(buf+c,0xE3C338FF); c+=4;
            write_be32(buf+c,0xE1A01461); c+=4;
            write_be32(buf+c,0xE0211423); c+=4;
            // extract 15-bit length, store it
            write_be32(buf+c,0xE1A01881); c+=4;
            write_be32(buf+c,0xE1A018A1); c+=4;
            write_be32(buf+c,0xE1C810B0); c+=4;
            // data ptr = rxBuf + data_offset
            write_be32(buf+c,0xE5940040); c+=4;
            write_be32(buf+c,0xE0870000); c+=4;
            write_be32(buf+c,0xE58A0000); c+=4;
            // advance data_offset by ALIGN8(len + 2), 772D pads to 8 with 0xEEEE prefix
            write_be32(buf+c,0xE2811002); c+=4;
            write_be32(buf+c,0xE2811007); c+=4;
            write_be32(buf+c,0xE3C11007); c+=4;
            write_be32(buf+c,0xE5940040); c+=4;
            write_be32(buf+c,0xE0800001); c+=4;
            write_be32(buf+c,0xE5840040); c+=4;
            // skip 0xEEEE prefix in output pointer
            write_be32(buf+c,0xE59A0000); c+=4;
            write_be32(buf+c,0xE2800002); c+=4;
            write_be32(buf+c,0xE58A0000); c+=4;
            // bump pkt_index, return 0
            write_be32(buf+c,0xE2899001); c+=4;
            write_be32(buf+c,0xE584903C); c+=4;
            write_be32(buf+c,0xE3A00000); c+=4;
            write_be32(buf+c,0xE8BD87F0); c+=4;

            // bulk_read - fix up forward branches
            u32 rx_bulk = c;
            write_be32(buf+rx_ble, 0xDA000000|((((s32)((ARM32_VA_BASE+rx_bulk-ARM32_ELF_BASE)-(ARM32_VA_BASE+rx_ble-ARM32_ELF_BASE)-8))>>2)&0xFFFFFF));
            write_be32(buf+rx_bge, 0xAA000000|((((s32)((ARM32_VA_BASE+rx_bulk-ARM32_ELF_BASE)-(ARM32_VA_BASE+rx_bge-ARM32_ELF_BASE)-8))>>2)&0xFFFFFF));
            // reset state
            write_be32(buf+c,0xE3A09000); c+=4;
            write_be32(buf+c,0xE584903C); c+=4;
            write_be32(buf+c,0xE5849040); c+=4;
            // USB bulk read on endpoint 0x82
            write_be32(buf+c,0xE5940000); c+=4;
            write_be32(buf+c,0xE3A01082); c+=4;
            write_be32(buf+c,0xE1A02005); c+=4;
            write_be32(buf+c,0xE1A03007); c+=4;
            write_be32(buf+c,0xEB000000|((((s32)(0x13AA5170-(ARM32_VA_BASE+c-ARM32_ELF_BASE)-8))>>2)&0xFFFFFF)); c+=4;
            // need at least 8 bytes for trailer
            write_be32(buf+c,0xE3500008); c+=4;
            u32 rx_blt = c;
            write_be32(buf+c,0xBA000000); c+=4;
            write_be32(buf+c,0xE5840038); c+=4;
            write_be32(buf+c,0xE1A06000); c+=4;
            // loop back to parse the data we just read
            write_be32(buf+c,arm32_branch(ARM32_VA_BASE+c-ARM32_ELF_BASE, ARM32_VA_BASE+rx_ble+4-ARM32_ELF_BASE)); c+=4;

            // error - clear rxSizeLeft, return -0x17
            u32 rx_err = c;
            write_be32(buf+rx_blt, 0xBA000000|((((s32)((ARM32_VA_BASE+rx_err-ARM32_ELF_BASE)-(ARM32_VA_BASE+rx_blt-ARM32_ELF_BASE)-8))>>2)&0xFFFFFF));
            write_be32(buf+c,0xE3A00000); c+=4;
            write_be32(buf+c,0xE5840038); c+=4;
            write_be32(buf+c,0xE3E00016); c+=4;
            write_be32(buf+c,0xE8BD87F0); c+=4;

            // literal pool - MFB size global
            u32 rx_lit = c;
            write_be32(buf+c,0x13AB6B60); c+=4;
            {
                u32 pc_val = ARM32_VA_BASE + rx_ldr_mfb - ARM32_ELF_BASE + 8;
                u32 lit_va = ARM32_VA_BASE + rx_lit - ARM32_ELF_BASE;
                write_be32(buf+rx_ldr_mfb, 0xE59F5000 | ((lit_va-pc_val)&0xFFF));
            }

            write_be32(buf + ARM32_FO_RECVPACKET, arm32_branch(
                ARM32_VA_BASE + ARM32_FO_RECVPACKET - ARM32_ELF_BASE, rx_va));
            applied++;
            printf("  RX parser: %u bytes, patched axRecvPacket\n", c - rx_fo);
            printf("  total injected: %u / %u bytes\n", c - CAVE_FILE_START, CAVE_SIZE);
        }
    }

    if (applied == 0) { printf("772D ARM32: all patches already applied\n"); return 1; }

    t->contents[index].type = 1;
    SHA1(buf, size, hash);
    memcpy(cr[index].hash, hash, 20);
    printf("772D ARM32: %d new + %d existing on content #%d\n", applied, existing, index);
    return 0;
}

s32 patch_772d_thumb(IOS *ios)
{
    tmd *t = (tmd *)SIGNATURE_PAYLOAD(ios->tmd);
    tmd_content *cr = TMD_CONTENTS(t);
    u8 hash[20];
    int vidpid_hits = 0;

    // thumb driver has no code caves, so only VID:PID + register patches here
    // full protocol translation only happens in the ARM32 driver

    printf("772D: scanning contents for VID:PID...\n");
    for (int i = 0; i < ios->content_count; i++) {
        if (!ios->decrypted_buffer[i]) continue;
        u8 *buf = ios->decrypted_buffer[i];
        u32 sz = (u32)cr[i].size;

        s32 off = find_pattern(buf, sz, pat_ios58_vidpid, sizeof(pat_ios58_vidpid));
        if (off < 0) {
            u8 pat_772b[] = { 0x0B,0x95,0x77,0x2B, 0x00,0xFF,0xFF,0xFF };
            off = find_pattern(buf, sz, pat_772b, sizeof(pat_772b));
        }
        if (off >= 0) {
            printf("  content #%d: VID:PID @ 0x%04X -> 0x1790\n", i, off + 2);
            buf[off + 2] = 0x17;
            buf[off + 3] = 0x90;

            t->contents[i].type = 1;
            SHA1(buf, sz, hash);
            memcpy(cr[i].hash, hash, 20);
            vidpid_hits++;
            continue;
        }
        u8 done[] = { 0x0B,0x95,0x17,0x90, 0x00,0xFF,0xFF,0xFF };
        if (find_pattern(buf, sz, done, sizeof(done)) >= 0) {
            printf("  content #%d: VID:PID already 0x1790\n", i);
            vidpid_hits++;
        }
    }

    if (vidpid_hits == 0) { printf("772D: no VID:PID found in Thumb contents\n"); return -1; }
    printf("772D: VID:PID patched/verified in %d content(s)\n", vidpid_hits);

    s32 eth = find_content_with(ios, pat_ios58_rxc_318, sizeof(pat_ios58_rxc_318));
    if (eth < 0)
        eth = find_content_with(ios, pat_ios58_rxc_118, sizeof(pat_ios58_rxc_118));
    if (eth < 0) {
        if (find_content_with(ios, pat_ios58_rxc_018, sizeof(pat_ios58_rxc_018)) >= 0) {
            printf("772D: register patches already applied\n");
            return (vidpid_hits > 0) ? 0 : 1;
        }
        return -1;
    }

    printf("772D: ETH driver is content #%d\n", eth);
    u8 *buf = ios->decrypted_buffer[eth];
    u32 size = (u32)cr[eth].size;
    int applied = 0, existing = 0;
    s32 r;

    r = patch_block(buf, size, pat_ios58_rxc_318, pat_ios58_rxc_018, sizeof(pat_ios58_rxc_318), "rxctrl EHC");
    if (r < 0) r = patch_block(buf, size, pat_ios58_rxc_118, pat_ios58_rxc_018, sizeof(pat_ios58_rxc_118), "rxctrl EHC (fix 0x118)");
    if (r < 0) return -1;
    if (r == 0) applied++; else existing++;

    r = patch_block(buf, size, pat_ios58_rxc_218, pat_ios58_rxc_018, sizeof(pat_ios58_rxc_218), "rxctrl OH0");
    if (r < 0) r = patch_block(buf, size, pat_ios58_rxc_118, pat_ios58_rxc_018, sizeof(pat_ios58_rxc_118), "rxctrl OH0 (fix 0x118)");
    if (r < 0) return -1;
    if (r == 0) applied++; else existing++;

    {
        int n = count_pattern(buf, size, pat_ios58_swrst, sizeof(pat_ios58_swrst));
        int n_done = count_pattern(buf, size, pat_ios58_swrst_new, sizeof(pat_ios58_swrst_new));
        if (n == 1 && n_done == 0) {
            r = patch_block(buf, size, pat_ios58_swrst, pat_ios58_swrst_new, sizeof(pat_ios58_swrst), "swrst down");
            if (r < 0) return -1;
            if (r == 0) applied++; else existing++;
        } else if (n == 0 && n_done == 1) {
            printf("  swrst down: already done\n"); existing++;
        } else if (n > 1) {
            printf("  swrst down: %d matches, skipping\n", n);
        } else {
            printf("  swrst down: not found\n"); return -1;
        }
    }

    if (applied > 0) {
        t->contents[eth].type = 1;
        SHA1(buf, size, hash);
        memcpy(cr[eth].hash, hash, 20);
    }

    printf("772D Thumb: %d new + %d existing on content #%d\n", applied, existing, eth);
    return (applied > 0 || vidpid_hits > 0) ? 0 : 1;
}
