//patches IOS modules for AX88772B/C/D USB ethernet support on vWii

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ogcsys.h>
#include <gccore.h>
#include <wiiuse/wpad.h>
#include <network.h>
#include <fat.h>
#include <sdcard/wiisd_io.h>
#include <ogc/usb.h>

#include "IOSPatcher.h"
#include "identify.h"
#include "isfs.h"
#include "sha1.h"
#include "tools.h"
#include "memory/mem2.hpp"
#include "patch_772bc.h"
#include "patch_772d.h"

extern s32 get_IOS(IOS **ios, u32 iosnr, u32 revision);
extern void encrypt_IOS(IOS *ios);
extern void forge_tmd(signed_blob *s_tmd);
extern s32 install_IOS(IOS *ios, bool skipticket);

#define IOS58_NR 58

#define ROUND_UP(x, n) (-(-(x) & -(n)))

#define ASIX_VID 0x0B95
#define PID_772A 0x7720
#define PID_772B 0x772B
#define PID_772D 0x1790

static u16 detected_pid = 0;
static u16 detected_bcd = 0;
static bool skip_adapter_check = false;

static void detect_adapter(void)
{
    usb_device_entry devices[16];
    u8 count = 0;
    u8 classes[] = { 0xFF, 0 };

    detected_pid = 0;
    detected_bcd = 0;

    for (int c = 0; c < 2; c++) {
        count = 0;
        if (USB_GetDeviceList(devices, 16, classes[c], &count) < 0)
            continue;
        for (int i = 0; i < count; i++) {
            if (devices[i].vid != ASIX_VID)
                continue;
            detected_pid = devices[i].pid;

            usb_devdesc desc ATTRIBUTE_ALIGN(32);
            memset(&desc, 0, sizeof(desc));
            if (USB_GetDeviceDescription(devices[i].device_id, &desc) >= 0)
                detected_bcd = desc.bcdDevice;
            return;
        }
    }
}

static const char *adapter_name(void)
{
    if (detected_pid == PID_772A) return "AX88772 (stock)";
    if (detected_pid == PID_772B) {
        if (detected_bcd == 0x0002) return "AX88772C";
        return "AX88772B";
    }
    if (detected_pid == PID_772D) return "AX88772D";
    if (detected_pid != 0) return "Unknown ASIX";
    return NULL;
}

static s32 do_patch_and_install(u32 iosnr)
{
    s32 ver = checkIOS(iosnr);
    if (ver < 0) return -1;

    IOS *ios = NULL;
    s32 ret = get_IOS(&ios, iosnr, (u32)ver);
    if (ret < 0) return ret;

    s32 arm_ret = patch_arm32_ethernet(ios);
    s32 thumb_ret = patch_thumb_ethernet(ios);

    if (arm_ret < 0 && thumb_ret < 0) { free_IOS(&ios); return -1; }
    if (arm_ret == 1 && (thumb_ret == 1 || thumb_ret < 0)) { free_IOS(&ios); return 1; }
    ret = (arm_ret == 0 || thumb_ret == 0) ? 0 : 1;
    if (ret == 1) { free_IOS(&ios); return 1; }

    forge_tmd(ios->tmd);
    encrypt_IOS(ios);
    ret = install_IOS(ios, false);
    free_IOS(&ios);
    return ret;
}

static s32 do_patch_772d_and_install(u32 iosnr)
{
    s32 ver = checkIOS(iosnr);
    if (ver < 0) return -1;

    IOS *ios = NULL;
    s32 ret = get_IOS(&ios, iosnr, (u32)ver);
    if (ret < 0) return ret;

    s32 arm_ret = patch_772d_arm32(ios);
    s32 thumb_ret = patch_772d_thumb(ios);

    if (arm_ret < 0 && thumb_ret < 0) { free_IOS(&ios); return -1; }
    if (arm_ret == 1 && (thumb_ret == 1 || thumb_ret < 0)) { free_IOS(&ios); return 1; }
    ret = (arm_ret == 0 || thumb_ret == 0) ? 0 : 1;
    if (ret == 1) { free_IOS(&ios); return 1; }

    forge_tmd(ios->tmd);
    encrypt_IOS(ios);
    ret = install_IOS(ios, false);
    free_IOS(&ios);
    return ret;
}

static const u32 eth_ios[] = {
    31, 33, 34, 35, 36, 37, 38,
    41, 43, 45, 46, 48,
    53, 55, 56, 57, 58, 59,
    62, 80
};
#define ETH_IOS_COUNT (sizeof(eth_ios) / sizeof(eth_ios[0]))

typedef struct {
    u32 header_len;
    u16 type;
    u16 padding;
    u32 certs_len;
    u32 crl_len;
    u32 tik_len;
    u32 tmd_len;
    u32 data_len;
    u32 footer_len;
} ATTRIBUTE_PACKED wad_header_t;

static s32 write_wad_file(const char *path, IOS *ios)
{
    u32 data_size = 0;
    for (int i = 0; i < ios->content_count; i++)
        data_size += ROUND_UP(ios->buffer_size[i], 64);

    wad_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.header_len = 0x20;
    hdr.type = 0x4973;
    hdr.certs_len = ios->certs_size;
    hdr.crl_len = ios->crl_size;
    hdr.tik_len = ios->ticket_size;
    hdr.tmd_len = ios->tmd_size;
    hdr.data_len = data_size;

    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;

    u8 zeros[64];
    memset(zeros, 0, 64);

    #define PAD_WRITE(ptr, len) do { \
        fwrite(ptr, 1, len, fp); \
        u32 p = ROUND_UP(len, 64) - (len); \
        if (p > 0) fwrite(zeros, 1, p, fp); \
    } while(0)

    PAD_WRITE(&hdr, sizeof(hdr));
    PAD_WRITE(ios->certs, ios->certs_size);
    if (ios->crl && ios->crl_size > 0)
        PAD_WRITE(ios->crl, ios->crl_size);
    PAD_WRITE(ios->ticket, ios->ticket_size);
    PAD_WRITE(ios->tmd, ios->tmd_size);

    for (int i = 0; i < ios->content_count; i++)
        fwrite(ios->encrypted_buffer[i], 1, ROUND_UP(ios->buffer_size[i], 64), fp);

    fclose(fp);
    #undef PAD_WRITE
    return 0;
}

static s32 dump_single_ios(u32 iosnr)
{
    s32 ver = checkIOS(iosnr);
    if (ver < 0) return -1;

    IOS *ios = NULL;
    s32 ret = Nand_Read_into_memory(&ios, iosnr, (u32)ver);
    if (ret < 0 || !ios || !ios->tmd) { if (ios) free_IOS(&ios); return -1; }

    encrypt_IOS(ios);

    char path[64];
    sprintf(path, "sd:/wad/IOS%u-64-v%u.wad", iosnr, (u32)ver);
    ret = write_wad_file(path, ios);
    free_IOS(&ios);
    return ret;
}


static void network_test(void)
{
    printf("\nReloading IOS58...\n");
    WPAD_Shutdown();
    IOS_ReloadIOS(IOS58_NR);

    PAD_Init();
    WPAD_Init();
    WPAD_SetDataFormat(WPAD_CHAN_0, WPAD_FMT_BTNS_ACC_IR);

    printf("Waiting for controller...");
    for (int w = 0; w < 40; w++) {
        WPAD_ScanPads();
        u32 type = 0;
        if (WPAD_Probe(0, &type) == WPAD_ERR_NONE) break;
        PAD_ScanPads();
        if (PAD_ButtonsHeld(0)) break;
        usleep(100000);
        if (w % 5 == 0) printf(".");
    }
    printf("\n");

    u32 pressed;
    for (;;) {
        printf("\nInitializing network");
        s32 net = -1;
        for (int t = 0; t < 50; t++) {
            net = net_init();
            if (net == 0) break;
            if (t % 5 == 0) printf(".");
            usleep(200000);
        }

        if (net < 0) {
            printf("\nNetwork init failed: %d\n", net);
            printf("Make sure the adapter is plugged in.\n");
        } else {
            u32 ip = net_gethostip();
            if (ip == 0) {
                printf("\nNo IP assigned (DHCP timeout?)\n");
            } else {
                printf("\nLocal: %u.%u.%u.%u\n",
                    (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
                    (ip >> 8) & 0xFF, ip & 0xFF);

                printf("WAN:   ");
                s32 sock = net_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
                if (sock >= 0) {
                    struct hostent *he = net_gethostbyname("api.ipify.org");
                    if (he && he->h_length > 0) {
                        struct sockaddr_in sa;
                        memset(&sa, 0, sizeof(sa));
                        sa.sin_family = AF_INET;
                        sa.sin_port = htons(80);
                        memcpy(&sa.sin_addr, he->h_addr_list[0], he->h_length);

                        if (net_connect(sock, (struct sockaddr *)&sa, sizeof(sa)) >= 0) {
                            const char *req =
                                "GET / HTTP/1.0\r\n"
                                "Host: api.ipify.org\r\n"
                                "Connection: close\r\n\r\n";
                            net_send(sock, req, strlen(req), 0);

                            char resp[512];
                            int total = 0, n, idle = 0;
                            while (total < (int)sizeof(resp) - 1 && idle < 250) {
                                n = net_recv(sock, resp + total, sizeof(resp) - 1 - total, 0);
                                if (n > 0) { total += n; idle = 0; }
                                else if (n == 0) break;
                                else { usleep(20000); idle++; }
                            }
                            resp[total] = '\0';

                            char *body = strstr(resp, "\r\n\r\n");
                            if (body) body += 4;
                            else { body = strstr(resp, "\n\n"); if (body) body += 2; }
                            if (body) {
                                char *end = body;
                                while (*end && *end != '\r' && *end != '\n' && *end != ' ') end++;
                                *end = '\0';
                                printf("%s\n", *body ? body : "(empty)");
                            } else printf("(bad response)\n");
                        } else printf("(connect failed)\n");
                    } else printf("(DNS failed)\n");
                    net_close(sock);
                } else printf("(socket failed)\n");
            }
            net_deinit();
        }

        printf("\nPress B to retry, any other button to go back.\n");
        pressed = 0;
        waitforbuttonpress(&pressed, NULL);
        if (pressed != WPAD_BUTTON_B) break;
    }
}

static void init_video(void)
{
    VIDEO_Init();
    GXRModeObj *rmode = VIDEO_GetPreferredMode(NULL);
    void *xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();
    CON_InitEx(rmode, 24, 32, rmode->fbWidth - 32, rmode->xfbHeight - 48);
    VIDEO_ClearFrameBuffer(rmode, xfb, COLOR_BLACK);
}

static bool is_patchable(void)
{
    return (detected_pid == PID_772B || detected_pid == PID_772D);
}

static void draw_adapter_status(void)
{
    int cols, rows;
    CON_GetMetrics(&cols, &rows);
    char line[64];
    if (is_patchable())
        sprintf(line, "Adapter found! %s.", adapter_name());
    else if (detected_pid == PID_772A)
        sprintf(line, "Adapter found! %s.", adapter_name());
    else
        sprintf(line, "Please plug in your adapter...");
    int len = (int)strlen(line);
    int col = cols - len;
    if (col < 1) col = 1;
    // clear the whole row first, then print right-aligned
    printf("\x1B[%d;1H%*s", rows - 1, cols, "");
    printf("\x1B[%d;%dH%s", rows - 1, col, line);
}

static void draw_patch_line(void)
{
    char line[64];
    if (is_patchable())
        sprintf(line, "[A]     Patch for %s", adapter_name());
    else if (detected_pid == PID_772A)
        sprintf(line, "[A]     You don't need to patch, silly!");
    else
        sprintf(line, "[A]     Please plug in your adapter...");
    char padded[64];
    sprintf(padded, "%-40s", line);
    printf("\x1B[4;0H%s", padded);
}

static void print_center(const char *text)
{
    int cols, rows;
    CON_GetMetrics(&cols, &rows);
    int len = (int)strlen(text);
    int pad = (cols - len) / 2;
    if (pad < 0) pad = 0;
    printf("%*s%s\n", pad, "", text);
}

static void bail(const char *msg)
{
    printf("\n%s\n", msg);
    printf("Press any button to exit.\n");
    waitforbuttonpress(NULL, NULL);
    ISFS_Deinitialize();
    Reboot();
}

extern void __exception_setreload(int t);
extern s32 IOS_ReloadIOS(int version);

int main(int argc, char *argv[])
{
    __exception_setreload(10);
    s32 ret;

    init_video();
    MEM_init();

    printf("\n\n\n\n\n");
    print_center("This tool modifies IOS modules on your NAND!");
    print_center("I am not responsible for any damage done to your console!");
    printf("\n");
    print_center("Run the IOS dumper before patching!");
    print_center("Priiloader + Aroma are recommended!");
    printf("\n\n");
    print_center("Press any button to continue.");

    WPAD_Init();
    WPAD_SetDataFormat(WPAD_CHAN_0, WPAD_FMT_BTNS_ACC_IR);
    waitforbuttonpress(NULL, NULL);

    Patch_AHB();
    ret = (__IOS_LoadStartupIOS() == 0 && *(vu32 *)0xCD800064 == 0xFFFFFFFF);

    PatchIOS(true);
    usleep(1000);
    ISFS_Initialize();

    if (!ret)
        bail("AHBPROT not available. Launch from HBC.");

    Init_SD();

    u32 pressed = 0;

    for (;;) {
        if (!skip_adapter_check)
            detect_adapter();

        printf("\x1B[2J\x1B[H");
        printf("=== vWii Ethernet Patcher ===\n\n");

        printf("[+]     Dump stock IOS to SD (do this first!)\n");
        printf("[A]     Patch\n");
        printf("[B]     Test network\n");
        printf("[HOME]  Exit\n\n");

        if (!skip_adapter_check) {
            draw_patch_line();
            draw_adapter_status();
        }
        skip_adapter_check = false;
        printf("\x1B[8;0H");

        pressed = 0;
        int poll_counter = 0;
        u16 last_pid = detected_pid;
        for (;;) {
            WPAD_ScanPads();
            pressed = WPAD_ButtonsDown(0) | WPAD_ButtonsDown(1) |
                      WPAD_ButtonsDown(2) | WPAD_ButtonsDown(3);

            if (pressed) {
                if ((pressed & WPAD_BUTTON_A) && !is_patchable())
                    pressed = 0;
                else
                    break;
            }

            usleep(50000);
            poll_counter++;
            if (poll_counter >= 20) {
                poll_counter = 0;
                detect_adapter();
                if (detected_pid != last_pid) {
                    last_pid = detected_pid;
                    draw_patch_line();
                    draw_adapter_status();
                    printf("\x1B[8;0H");
                }
            }
        }

        if (pressed == WPAD_BUTTON_HOME) break;

        if (pressed == WPAD_BUTTON_PLUS) {
            int total = (int)ETH_IOS_COUNT;
            int dumped = 0, failed = 0;

            mkdir("sd:/wad", 0755);
            printf("\x1B[2J\x1B[H");
            printf("Dumping IOS to SD card... (0/%d)", total);

            for (u32 i = 0; i < ETH_IOS_COUNT; i++) {
                if (dump_single_ios(eth_ios[i]) == 0) dumped++; else failed++;
                int done = dumped + failed;
                printf("\x1B[1;1H");
                printf("Dumping IOS to SD card... (%d/%d)", done, total);
            }

            printf("\x1B[1;1H");
            if (failed == 0)
                printf("Dumping IOS to SD card... Done!       ");
            else
                printf("Dumping IOS to SD card... %d failed.   ", failed);

            printf("\n\nPress any button to continue.\n");
            waitforbuttonpress(NULL, NULL);
            continue;
        }

        if (pressed == WPAD_BUTTON_B) {
            network_test();
            if (*(vu32 *)0xCD800064 == 0xFFFFFFFF) {
                Patch_AHB();
                PatchIOS(true);
                usleep(1000);
                ISFS_Initialize();
                Init_SD();
            }
            skip_adapter_check = true;
            continue;
        }

        if (pressed != WPAD_BUTTON_A) continue;

        int mode_772d = (detected_pid == PID_772D);

        printf("\x1B[2J\x1B[H");
        printf("Patch IOS for %s?\n\n", adapter_name());
        printf("Press A to start, anything else to go back.\n");
        pressed = 0;
        waitforbuttonpress(&pressed, NULL);
        if (pressed != WPAD_BUTTON_A) continue;

        int total = (int)ETH_IOS_COUNT;
        int patched = 0, skipped = 0, failed = 0;
        u32 failed_ios[ETH_IOS_COUNT];

        printf("\x1B[2J\x1B[H");
        printf("Patching all relevant IOS's... (0/%d)\n\n", total);

        for (u32 i = 0; i < ETH_IOS_COUNT; i++) {
            ret = mode_772d ? do_patch_772d_and_install(eth_ios[i])
                            : do_patch_and_install(eth_ios[i]);

            if (ret == 0) patched++;
            else if (ret > 0) skipped++;
            else failed_ios[failed++] = eth_ios[i];

            int done = patched + skipped + failed;
            printf("\x1B[2J\x1B[H");
            printf("Patching all relevant IOS's... (%d/%d)\n\n", done, total);
        }

        printf("\x1B[2J\x1B[H");
        if (failed == 0)
            printf("Patching complete!\n");
        else
            printf("Patching finished with %d failures.\n", failed);
        printf("\n%d patched, %d already done, %d failed.\n", patched, skipped, failed);
        if (failed > 0) {
            printf("\nFailed: ");
            for (int f = 0; f < failed; f++)
                printf("IOS%u%s", failed_ios[f], f < failed - 1 ? ", " : "");
            printf("\n");
        }

        printf("\nPress any button to continue.\n");
        waitforbuttonpress(NULL, NULL);

        PatchIOS(true);
        usleep(1000);
        ISFS_Initialize();
        Init_SD();
    }

    Close_SD();
    ISFS_Deinitialize();
    Reboot();
    return 0;
}
