// Ethernet Patcher for the Wii and the vWii
//
// This tool changes the network driver (the "ETH module") inside the IOS of the console, so that
// these USB LAN adapters work:
//   - AX88772B / C / D
//   - AX88179  (the Nintendo Switch LAN adapter)
//   - AX88179A
//
// This file holds everything you see on the screen: the main menu and the menu entries.
// The real work (reading, patching and installing an IOS) is done in the other files.
// HOW_IT_WORKS.md gives a short tour through all files.
//
// The file is read best from top to bottom:
//   1. small helpers (screen, buttons)
//   2. which USB adapter is plugged in
//   3. the state of every IOS on the NAND
//   4. writing WAD files
//   5. menu entry [+]  Dump
//   6. menu entry [-]  Restore
//   7. menu entry [A]  Patch for the AX88179 / AX88179A
//   8. menu entry [A]  Patch for the AX88772B / C / D
//   9. menu entry [B]  Test network
//  10. the main screen and main()
//
// Number types of the Wii libraries: u8, u16, u32 and u64 are numbers without a sign of 8, 16, 32
// and 64 bits. s32 is a number with a sign of 32 bits. A function that returns an s32 usually returns
// 0 (or more) when it worked and a negative number for an error.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
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
#include "wad.h"
#include "sha1.h"
#include "tools.h"
#include "memory/mem2.hpp"
#include "patch_772bc.h"
#include "patch_772d.h"
#include "patch_179.h"
#include "platform.h"
#include "sigcheck.h"
#include "backup.h"

extern void __exception_setreload(int t);

#define VERSION "v2.0-beta"

// the IOS this tool runs on
#define IOS58_NR 58

// size of all IOS lists (there are 20 IOS with an ETH module)
#define MAX_IOS 32

// USB IDs of the adapters
#define ASIX_VID 0x0B95
#define PID_772A 0x7720     // the first AX88772, already works with the stock IOS
#define PID_772B 0x772B     // AX88772B and AX88772C
#define PID_772D 0x1790     // AX88772D, AX88179 and AX88179A all use this ID

// The AX88179A chooses its link speed itself, the driver cannot limit it to 100M.
// So the user is told to do that on the switch or router.
#define AX179A_HINT_1 "Tip: limit the switch / router port of this console to 100M."
#define AX179A_HINT_2 "With the AX88179A gigabit links were unreliable in tests."

// results of do_patch_179()
#define PATCH_DONE           0    // patched (and installed, or written to the SD card)
#define PATCH_ALREADY_DONE   1    // the IOS already has the AX88179 driver
#define PATCH_NO_MODULE      2    // the IOS has no ETH module this tool knows
#define PATCH_NO_BACKUP      3    // no backup on the SD card fits the IOS on the NAND
#define PATCH_NOT_INSTALLED -3    // the IOS is not on the NAND
                                  // any other negative number: an error

// results of dump_single_ios()
#define DUMP_STOCK           0    // saved as IOSn-64-vV.wad
#define DUMP_MODIFIED        2    // patched IOS on the NAND, not saved
#define DUMP_CUSTOM          3    // saved as IOSn-64-vV-custom.wad
#define DUMP_ERROR          -1


// ===================================================================================
//  1. Small helpers
// ===================================================================================

// The console understands the same escape codes as a terminal.
static void clear_screen(void)
{
    printf("\x1B[2J\x1B[H");
}

// moves the text cursor to a row and a column
static void move_cursor(int row, int col)
{
    printf("\x1B[%d;%dH", row, col);
}

static void print_spaces(int count)
{
    for (int i = 0; i < count; i++)
        printf(" ");
}

static void press_any_button(void)
{
    printf("\nPress any button to continue.\n");
    waitforbuttonpress(NULL, NULL);
}

// waits for one button and returns it
static u32 get_button(void)
{
    u32 pressed = 0;
    waitforbuttonpress(&pressed, NULL);
    return pressed;
}

// prints a list like "IOS31, IOS33, IOS34"
static void print_ios_list(const u32 *list, int count)
{
    for (int i = 0; i < count; i++) {
        printf("IOS%u", list[i]);
        if (i < count - 1)
            printf(", ");
    }
}

// prints a line in the middle of the screen
static void print_center(const char *text)
{
    int cols, rows;
    CON_GetMetrics(&cols, &rows);

    int left = (cols - (int)strlen(text)) / 2;
    print_spaces(left);
    printf("%s\n", text);
}

// Asks for [1], then [2], then [A]. This is used before anything is written to the NAND.
// Any other button cancels. Returns true if all three were pressed.
static bool confirm_sequence(void)
{
    printf("Press [1], then [2], then [A] to continue. Any other button cancels.\n");
    if (get_button() != WPAD_BUTTON_1)
        return false;

    printf("[1] ok - now [2]\n");
    if (get_button() != WPAD_BUTTON_2)
        return false;

    printf("[2] ok - now [A]\n");
    if (get_button() != WPAD_BUTTON_A)
        return false;

    return true;
}

// AHBPROT is the hardware access the patcher needs. The Homebrew Channel gives it to us.
static bool have_ahbprot(void)
{
    return *(vu32 *)0xCD800064 == 0xFFFFFFFF;
}

// After an IOS reload or an install the NAND and the SD card have to be opened again.
static void reinit_nand(void)
{
    PatchIOS(true);
    usleep(1000);
    ISFS_Initialize();
    Init_SD();
}

// shows an error and leaves the patcher
static void bail(const char *message)
{
    printf("\n%s\n", message);
    printf("Press any button to exit.\n");
    waitforbuttonpress(NULL, NULL);
    ISFS_Deinitialize();
    Reboot();
}


// ===================================================================================
//  2. Which USB adapter is plugged in
// ===================================================================================

// What we know about the plugged in adapter. detect_adapter() fills these variables.
static u16 adapter_pid = 0;           // USB product ID, 0 = no ASIX adapter plugged in
static u16 adapter_bcd = 0;           // device version, tells the AX88772B from the AX88772C
static char adapter_product[32];      // product name, only read for PID 0x1790

// The AX88772D, AX88179 and AX88179A all have the PID 0x1790. To tell them apart the device is opened
// and its product name ("AX88179", "AX88179A") is read. This is slow, so it is done once per device
// and then remembered:
static s32 read_device_id = -1;       // the USB device the information above was read from (-1 = none)
static bool adapter_read_ok = false;  // true if the product name or the version could be read

// Reads the device descriptor of a USB device. It holds the version and the number of the product
// name string. Returns true if it worked.
// (The Wii needs the memory of USB data to start at a multiple of 32 bytes, so the variables that are
// given to the USB functions are declared with ATTRIBUTE_ALIGN(32).)
static bool read_descriptor(s32 id, usb_devdesc *desc)
{
    memset(desc, 0, sizeof(*desc));
    return USB_GetDeviceDescription(id, desc) >= 0;
}

// Reads the product name (a text, e.g. "AX88179A") into adapter_product. Returns true if it worked.
static bool read_product_name(s32 fd, u8 string_number)
{
    static char text[64] ATTRIBUTE_ALIGN(32);

    memset(text, 0, sizeof(text));
    if (USB_GetAsciiString(fd, string_number, 0x0409, sizeof(text) - 1, text) <= 0)    // 0x0409 = English
        return false;

    strncpy(adapter_product, text, sizeof(adapter_product) - 1);
    adapter_product[sizeof(adapter_product) - 1] = 0;
    return true;
}

// Reads the product name and the version of a 0x1790 device (but only if that was not done before).
static void read_1790_info(s32 device_id)
{
    // already read from this device: nothing to do
    if (device_id == read_device_id && adapter_read_ok)
        return;

    // Start from scratch. If reading fails (right after an IOS reload the USB stack can still be
    // busy), nothing is remembered and it is simply tried again at the next check.
    read_device_id = -1;
    adapter_read_ok = false;
    adapter_product[0] = 0;
    adapter_bcd = 0;

    s32 fd = -1;
    if (USB_OpenDevice(device_id, ASIX_VID, PID_772D, &fd) < 0)
        return;

    usb_devdesc desc ATTRIBUTE_ALIGN(32);
    u8 string_number = 2;                 // the AX88179 has its product name at string 2
    bool desc_ok = read_descriptor(fd, &desc);
    if (desc_ok) {
        adapter_bcd = desc.bcdDevice;
        if (desc.iProduct != 0)
            string_number = desc.iProduct;
    }

    bool name_ok = read_product_name(fd, string_number);
    USB_CloseDevice(&fd);

    if (desc_ok || name_ok) {
        adapter_read_ok = true;
        read_device_id = device_id;
    }
}

// Looks through the USB devices for the first one made by ASIX. Returns true if there is one.
static bool find_asix_device(usb_device_entry *found)
{
    usb_device_entry devices[16];
    u8 classes[2] = { 0xFF, 0 };      // the adapters show up in one of these two device classes

    for (int c = 0; c < 2; c++) {
        u8 count = 0;
        if (USB_GetDeviceList(devices, 16, classes[c], &count) < 0)
            continue;

        for (int i = 0; i < count; i++) {
            if (devices[i].vid == ASIX_VID) {
                *found = devices[i];
                return true;
            }
        }
    }
    return false;
}

// Looks for an adapter and fills adapter_pid, adapter_bcd and adapter_product.
static void detect_adapter(void)
{
    usb_device_entry device;

    if (!find_asix_device(&device)) {
        // Nothing is plugged in. Forget what was read, so that a device plugged in later is read again.
        adapter_pid = 0;
        adapter_bcd = 0;
        read_device_id = -1;
        adapter_read_ok = false;
        return;
    }

    adapter_pid = device.pid;
    if (adapter_pid == PID_772D) {
        // (if this device was read before, the version and the name simply stay as they are)
        read_1790_info(device.device_id);
    } else {
        usb_devdesc desc ATTRIBUTE_ALIGN(32);
        adapter_bcd = 0;
        if (read_descriptor(device.device_id, &desc))
            adapter_bcd = desc.bcdDevice;
        read_device_id = -1;
    }
}

// libogc keeps its USB state across an IOS reload, and after long NAND or SD card work the device
// list can stay empty. Opening USB again brings the adapter back. The list is filled in the
// background, so we wait up to 2 seconds for it.
static void usb_refresh(void)
{
    USB_Deinitialize();
    USB_Initialize();
    read_device_id = -1;
    adapter_read_ok = false;

    for (int i = 0; i < 20; i++) {
        detect_adapter();
        if (adapter_pid != 0)
            break;
        usleep(100000);
    }
}

// true for the AX88179 and the AX88179A
static bool is_ax179(void)
{
    if (adapter_pid != PID_772D)
        return false;

    // the product name starts with "AX88179"
    if (adapter_product[0] != 0)
        return strncmp(adapter_product, "AX88179", 7) == 0;

    // no name could be read: the AX88179 has version 1.00 (the bytes can be swapped)
    return adapter_bcd == 0x0100 || adapter_bcd == 0x0001;
}

// true for the AX88179A (the driver switches it to its CDC-ECM mode)
static bool is_ax179a(void)
{
    return adapter_pid == PID_772D && strcmp(adapter_product, "AX88179A") == 0;
}

// true if this tool can patch for the plugged in adapter
static bool is_patchable(void)
{
    if (adapter_pid == PID_772B)
        return true;

    // a 0x1790 device is only patched after we know which chip it is
    if (adapter_pid == PID_772D)
        return adapter_read_ok;

    return false;
}

// name of the plugged in adapter for the screen, NULL if there is none
static const char *adapter_name(void)
{
    static char name[48];

    if (adapter_pid == PID_772A)
        return "AX88772 (stock)";

    if (adapter_pid == PID_772B) {
        if (adapter_bcd == 0x0002)
            return "AX88772C";
        return "AX88772B";
    }

    if (is_ax179()) {
        if (adapter_product[0] == 0 || strcmp(adapter_product, "AX88179") == 0)
            return "AX88179 (Switch LAN)";
        if (strcmp(adapter_product, "AX88179A") == 0)
            return "AX88179A (CDC-ECM mode)";
        // another chip of the AX88179 family
        snprintf(name, sizeof(name), "%.12s (untested)", adapter_product);
        return name;
    }

    if (adapter_pid == PID_772D) {
        if (adapter_read_ok)
            return "AX88772D";
        return "ASIX 0x1790 (reading...)";
    }

    if (adapter_pid != 0)
        return "Unknown ASIX";

    return NULL;
}


// ===================================================================================
//  3. The state of every IOS on the NAND
// ===================================================================================

// One entry per IOS in plat_ios[] (see platform.c), filled by refresh_status().
typedef struct {
    s32 version;      // -1 if the IOS is not installed
    int signature;    // SIG_NINTENDO (stock), SIG_FAKE (MODIFIED) or SIG_OTHER (custom)
    int backup;       // which backup on the SD card fits (BK_...), BK_NONE if none fits
} ios_info;

static ios_info ios_state[MAX_IOS];
static bool ios_selected[MAX_IOS];       // ticked in the AX88179 IOS list

// Reads the TMD (the content list) of an installed IOS. Free it with free().
// Returns NULL if the IOS is not installed.
static signed_blob *read_nand_tmd(u32 iosnr, u32 *len)
{
    signed_blob *tmd_data = NULL;
    u64 title_id = ((u64)1 << 32) | iosnr;     // IOS have the title ID 00000001-000000nn

    *len = 0;
    if (GetTMD(title_id, &tmd_data, len) < 0)
        return NULL;
    return tmd_data;
}

static u32 tmd_version(const signed_blob *tmd_data)
{
    tmd *t = (tmd *)SIGNATURE_PAYLOAD(tmd_data);
    return t->title_version;
}

// "stock", "MODIFIED", "custom" or "missing"
static const char *state_name(const ios_info *info)
{
    if (info->version < 0)
        return "missing";
    return sig_class_name(info->signature);
}

// Reads the state of every IOS again. Called at the start and after every change.
static void refresh_status(void)
{
    for (u32 k = 0; k < plat_ios_count && k < MAX_IOS; k++) {
        u32 iosnr = plat_ios[k];
        ios_info *info = &ios_state[k];

        info->version = -1;
        info->signature = SIG_OTHER;
        info->backup = BK_NONE;

        u32 len;
        signed_blob *tmd_data = read_nand_tmd(iosnr, &len);
        if (tmd_data == NULL)
            continue;             // not installed

        info->version = tmd_version(tmd_data);
        info->signature = tmd_sig_class(tmd_data, len);

        backup_match match;
        if (backup_find(iosnr, (u32)info->version, tmd_data, &match) == 0)
            info->backup = match.kind;
        free(tmd_data);

        write_log("status IOS%u v%d: %s, backup %s", iosnr, (int)info->version,
                  sig_class_name(info->signature), backup_kind_name(info->backup));
    }
}

// true if the IOS is installed but has no fitting backup (shown with a '*')
static bool backup_missing(const ios_info *info)
{
    return info->version >= 0 && info->backup == BK_NONE;
}


// ===================================================================================
//  4. Writing WAD files
// ===================================================================================

// A WAD file starts with this header. It is followed by the certificates, the ticket, the TMD
// and the encrypted contents. Every part starts at a multiple of 64 bytes.
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

// rounds up to the next multiple of 64
static u32 round_up_64(u32 value)
{
    return (value + 63) / 64 * 64;
}

// writes data and fills it up with zeros to a multiple of 64 bytes
static void write_padded(FILE *fp, const void *data, u32 len)
{
    static const u8 zeros[64];

    fwrite(data, 1, len, fp);

    u32 pad = round_up_64(len) - len;
    if (pad > 0)
        fwrite(zeros, 1, pad, fp);
}

// writes an IOS (already encrypted) as a WAD file. 0 ok, -1 the file could not be created
static s32 write_wad_file(const char *path, IOS *ios)
{
    u32 data_size = 0;
    for (int i = 0; i < ios->content_count; i++)
        data_size += round_up_64(ios->buffer_size[i]);

    wad_header_t header;
    memset(&header, 0, sizeof(header));
    header.header_len = 0x20;
    header.type = 0x4973;                    // "Is"
    header.certs_len = ios->certs_size;
    header.crl_len = ios->crl_size;
    header.tik_len = ios->ticket_size;
    header.tmd_len = ios->tmd_size;
    header.data_len = data_size;

    FILE *fp = fopen(path, "wb");
    if (fp == NULL)
        return -1;

    write_padded(fp, &header, sizeof(header));
    write_padded(fp, ios->certs, ios->certs_size);
    if (ios->crl != NULL && ios->crl_size > 0)
        write_padded(fp, ios->crl, ios->crl_size);
    write_padded(fp, ios->ticket, ios->ticket_size);
    write_padded(fp, ios->tmd, ios->tmd_size);

    // the encrypted buffers are already a multiple of 64 bytes long
    for (int i = 0; i < ios->content_count; i++)
        fwrite(ios->encrypted_buffer[i], 1, round_up_64(ios->buffer_size[i]), fp);

    fclose(fp);
    return 0;
}

// creates sd:/wad, sd:/wad/vwii (or wii) and sd:/wad/vwii/ax179
static void make_wad_dirs(void)
{
    char dir[64];

    mkdir("sd:/wad", 0755);
    mkdir(plat_wad_dir(), 0755);
    snprintf(dir, sizeof(dir), "%s/ax179", plat_wad_dir());
    mkdir(dir, 0755);
}


// ===================================================================================
//  5. [+] Dump: save every IOS to the SD card
// ===================================================================================

// Saves one IOS from the NAND to the SD card:
//   - a stock IOS goes to IOSn-64-vV.wad
//   - a custom IOS (changed by another tool, e.g. WiiLink) goes to IOSn-64-vV-custom.wad
//   - a MODIFIED IOS (patched) is never saved, so a good backup is never replaced by a patched one
// Returns DUMP_STOCK, DUMP_CUSTOM, DUMP_MODIFIED or DUMP_ERROR.
static s32 dump_single_ios(u32 iosnr)
{
    s32 version = checkIOS(iosnr);
    if (version < 0)
        return DUMP_ERROR;

    IOS *ios = NULL;
    s32 ret = Nand_Read_into_memory(&ios, iosnr, (u32)version);
    if (ret < 0 || ios == NULL || ios->tmd == NULL) {
        if (ios != NULL)
            free_IOS(&ios);
        return DUMP_ERROR;
    }

    int signature = tmd_sig_class(ios->tmd, ios->tmd_size);
    if (signature == SIG_FAKE) {
        free_IOS(&ios);
        return DUMP_MODIFIED;
    }

    int kind = BK_CUSTOM;
    if (signature == SIG_NINTENDO)
        kind = BK_STOCK;

    char path[96];
    backup_path(path, sizeof(path), kind, iosnr, (u32)version);

    encrypt_IOS(ios);
    ret = write_wad_file(path, ios);
    free_IOS(&ios);

    if (ret < 0)
        return DUMP_ERROR;
    if (kind == BK_STOCK)
        return DUMP_STOCK;
    return DUMP_CUSTOM;
}

// How many entries of results[] are equal to wanted (count = number of entries)
static int count_results(const s32 *results, int count, s32 wanted)
{
    int found = 0;
    for (int i = 0; i < count; i++) {
        if (results[i] == wanted)
            found++;
    }
    return found;
}

// Prints the title and the list of all IOS that have the wanted result, e.g.
// "Failed: IOS31, IOS33". Prints nothing if there is no such IOS.
static void print_dump_list(const char *title, const s32 *results, s32 wanted)
{
    u32 list[MAX_IOS];
    int count = 0;

    for (u32 i = 0; i < plat_ios_count; i++) {
        if (results[i] == wanted) {
            list[count] = plat_ios[i];
            count++;
        }
    }
    if (count == 0)
        return;

    printf("\n\n%s", title);
    print_ios_list(list, count);
}

// the first line of the dump screen
static void print_dump_progress(int done, int total)
{
    printf("Dumping IOS to %s/ ... (%d/%d)", plat_wad_dir(), done, total);
}

static void dump_all_ios(void)
{
    int total = (int)plat_ios_count;
    s32 results[MAX_IOS];         // what dump_single_ios() returned for every IOS

    make_wad_dirs();
    clear_screen();
    print_dump_progress(0, total);

    for (int i = 0; i < total; i++) {
        move_cursor(3, 1);
        printf("Current: IOS%u        ", plat_ios[i]);

        results[i] = dump_single_ios(plat_ios[i]);
        write_log("dump IOS%u -> %d", plat_ios[i], (int)results[i]);

        move_cursor(1, 1);
        print_dump_progress(i + 1, total);
    }

    int stock = count_results(results, total, DUMP_STOCK);
    int custom = count_results(results, total, DUMP_CUSTOM);
    int failed = count_results(results, total, DUMP_ERROR);

    // clear the "Current: IOSxx" line
    move_cursor(3, 1);
    print_spaces(40);

    move_cursor(1, 1);
    if (failed == 0)
        printf("Dumping IOS to %s/ ... Done!       ", plat_wad_dir());
    else
        printf("Dumping IOS to %s/ ... %d failed.   ", plat_wad_dir(), failed);

    move_cursor(3, 1);
    printf("%d dumped (%d stock, %d custom).", stock + custom, stock, custom);

    print_dump_list("Custom (not signed by Nintendo, e.g. WiiLink), saved as *-custom.wad: ", results, DUMP_CUSTOM);
    print_dump_list("Modified on NAND, NOT dumped (existing backups kept): ", results, DUMP_MODIFIED);
    print_dump_list("Failed: ", results, DUMP_ERROR);
    if (failed > 0)
        printf("\n(details in " LOG_FILE ")");
    printf("\n");
    press_any_button();
}

// ===================================================================================
//  6. [-] Restore: write the backups of patched IOS back to the NAND
// ===================================================================================

// one IOS that will be restored
typedef struct {
    u32 iosnr;
    char path[80];        // the backup on the SD card
} restore_job;

// Looks at every MODIFIED (patched) IOS and finds its backup:
//   - the backup it was made from (stock or custom), or
//   - if another tool changed more than the ETH module: a stock backup of the same version.
// Fills jobs[] and returns how many were found. *missing = MODIFIED IOS without any backup.
static int find_restore_backups(restore_job *jobs, int *missing)
{
    int count = 0;
    *missing = 0;

    for (u32 i = 0; i < plat_ios_count; i++) {
        u32 iosnr = plat_ios[i];

        u32 len;
        signed_blob *tmd_data = read_nand_tmd(iosnr, &len);
        if (tmd_data == NULL)
            continue;                 // not installed

        u32 version = tmd_version(tmd_data);
        if (tmd_sig_class(tmd_data, len) != SIG_FAKE) {
            free(tmd_data);
            continue;                 // stock or custom: nothing to restore
        }

        backup_match match;
        bool found = false;
        if (backup_find(iosnr, version, tmd_data, &match) == 0) {
            printf(" IOS%-3u v%-5u -> %s backup (state before patching)\n",
                   iosnr, (unsigned)version, backup_kind_name(match.kind));
            found = true;
        } else if (backup_find_nintendo(iosnr, version, &match) == 0) {
            printf(" IOS%-3u v%-5u -> stock backup (changed by another tool, FULL stock restore)\n",
                   iosnr, (unsigned)version);
            found = true;
        } else {
            printf(" IOS%-3u v%-5u -> no matching backup in %s/\n",
                   iosnr, (unsigned)version, plat_wad_dir());
            (*missing)++;
        }
        free(tmd_data);

        if (found) {
            jobs[count].iosnr = iosnr;
            strcpy(jobs[count].path, match.path);
            count++;
        }
    }
    return count;
}

// reads one backup from the SD card and installs it. 0 ok, <0 error
static s32 restore_one(restore_job *job)
{
    s32 version = checkIOS(job->iosnr);
    IOS *ios = NULL;
    s32 ret = -1;

    if (version >= 0)
        ret = Wad_Read_into_memory(job->path, &ios, job->iosnr, (u32)version);
    if (ret >= 0 && ios != NULL) {
        encrypt_IOS(ios);
        ret = install_IOS(ios, false);
    }
    if (ios != NULL)
        free_IOS(&ios);

    write_log("restore IOS%u v%d from %s -> %d", job->iosnr, (int)version, job->path, (int)ret);
    return ret;
}

static void restore_stock_ios(void)
{
    restore_job jobs[MAX_IOS];
    int missing;

    clear_screen();
    printf("Restore IOS from %s/\n\n", plat_wad_dir());
    Init_SD();
    int count = find_restore_backups(jobs, &missing);

    if (count == 0) {
        if (missing > 0)
            printf("\nNothing to restore (no MODIFIED IOS with a matching backup).\n");
        else
            printf("\nNothing to restore (no MODIFIED IOS).\n");
        press_any_button();
        return;
    }

    printf("\n%d IOS will be written back to the NAND. Do NOT power off while it runs.\n", count);
    if (!confirm_sequence()) {
        printf("\nCancelled, nothing written.\n");
        press_any_button();
        return;
    }

    int ok = 0;
    int failed = 0;
    for (int k = 0; k < count; k++) {
        printf("IOS%u: ", jobs[k].iosnr);
        Init_SD();

        s32 ret = restore_one(&jobs[k]);
        if (ret == 0) {
            ok++;
            printf("restored\n");
        } else {
            failed++;
            printf("FAILED (%d)\n", (int)ret);
        }
    }

    printf("\n%d restored, %d failed.\n", ok, failed);
    if (failed > 0)
        printf("Do NOT power off yet - note this screen and keep " LOG_FILE ".\n");
    press_any_button();
}


// ===================================================================================
//  7. [A] Patch for the AX88179 / AX88179A
//
//  This works in two steps:
//    1. dry run: every selected IOS is patched and written to the SD card (sd:/wad/vwii/ax179/)
//    2. install: after [1] [2] [A] the same IOS are patched again and installed
// ===================================================================================

// Finds the backup that fits the IOS on the NAND and reads it into memory.
// Returns NULL if there is none.
static IOS *read_backup(u32 iosnr, u32 version, const signed_blob *nand_tmd, backup_match *match)
{
    if (backup_find(iosnr, version, nand_tmd, match) != 0)
        return NULL;

    IOS *ios = NULL;
    s32 ret = Wad_Read_into_memory(match->path, &ios, iosnr, version);
    if (ret < 0 || ios == NULL) {
        if (ios != NULL)
            free_IOS(&ios);
        write_log("IOS%u: %s not readable", iosnr, match->path);
        return NULL;
    }
    return ios;
}

// tells which backup the patch is made from (shown in the dry run)
static void print_patch_base(const backup_match *match, bool custom)
{
    if (match->kind == BK_CUSTOM || (match->kind == BK_LEGACY && custom))
        printf("  base: custom backup (not signed by Nintendo) - its changes are kept\n");
    else if (match->kind == BK_LEGACY)
        printf("  base: stock (old sd:/wad/) backup\n");
    else
        printf("  base: stock backup\n");
}

// Writes the patched IOS of the dry run to <wad folder>/ax179/. A custom IOS has "-custom" in its name.
static s32 write_dry_run_wad(IOS *ios, u32 iosnr, u32 version, bool custom)
{
    char path[96];
    const char *suffix = "";
    if (custom)
        suffix = "-custom";

    Init_SD();
    make_wad_dirs();
    snprintf(path, sizeof(path), "%s/ax179/IOS%u-64-v%u%s-ax179.wad", plat_wad_dir(), iosnr, version, suffix);
    return write_wad_file(path, ios);
}

// Patches one IOS for the AX88179. The patch is always made from the backup on the SD card that
// fits the IOS on the NAND, never from the NAND itself. (A custom IOS such as the one of WiiLink
// keeps its changes, because its custom backup is used.)
// install = false: dry run, the result is written to the SD card
// install = true:  the result is installed
// Returns one of the PATCH_... values, or a negative number on an error.
static s32 do_patch_179(u32 iosnr, bool install)
{
    Init_SD();

    // which version is on the NAND?
    u32 len;
    signed_blob *nand_tmd = read_nand_tmd(iosnr, &len);
    if (nand_tmd == NULL) {
        printf("  not installed, skipped\n");
        return PATCH_NOT_INSTALLED;
    }
    u32 version = tmd_version(nand_tmd);

    // the backup the patch is made from
    backup_match match;
    IOS *ios = read_backup(iosnr, version, nand_tmd, &match);
    free(nand_tmd);
    if (ios == NULL) {
        printf("  No backup in %s/ matches IOS%u v%u on the NAND.\n", plat_wad_dir(), iosnr, (unsigned)version);
        printf("  Patching is disabled until it exists: dump with [+] first.\n");
        return PATCH_NO_BACKUP;
    }

    // a backup with a broken (non-Nintendo) signature is a custom IOS
    bool custom = (match.sig != SIG_NINTENDO);
    if (!install)
        print_patch_base(&match, custom);

    // put in the new driver (patch_179.c)
    s32 ret = patch_179(ios);
    if (ret != 0) {
        free_IOS(&ios);
        return ret;
    }

    // the IOS is changed now, so its signature is replaced by a fake one
    forge_tmd(ios->tmd);
    encrypt_IOS(ios);

    if (install)
        ret = install_IOS(ios, false);
    else
        ret = write_dry_run_wad(ios, iosnr, version, custom);

    free_IOS(&ios);
    return ret;
}

// one entry of the IOS list, e.g. ">[x] IOS36  stock    "
static void print_select_entry(int k, int cursor)
{
    const ios_info *info = &ios_state[k];

    // 3 entries per line, 25 columns wide
    move_cursor(4 + k / 3, (k % 3) * 25);

    if (k == cursor)
        printf(">");
    else
        printf(" ");

    if (ios_selected[k])
        printf("[x] ");
    else
        printf("[ ] ");

    printf("IOS%-3u %-8s", plat_ios[k], state_name(info));

    if (backup_missing(info))
        printf("* ");
    else
        printf("  ");
}

// how many IOS are ticked in the list
static int count_selected(void)
{
    int selected = 0;
    for (u32 k = 0; k < plat_ios_count; k++) {
        if (ios_selected[k])
            selected++;
    }
    return selected;
}

// draws the whole IOS list with the help text below it
static void draw_select_menu(int cursor)
{
    int count = (int)plat_ios_count;
    int help_row = 5 + (count + 2) / 3;      // first line below the list

    move_cursor(1, 0);
    printf("Choose the IOS to patch (%s)\n\n", plat_name());
    for (int k = 0; k < count; k++)
        print_select_entry(k, cursor);

    move_cursor(help_row, 0);
    printf("* = no matching backup on SD (dump with [+] first)          ");
    move_cursor(help_row + 1, 0);
    printf("custom = not signed by Nintendo (e.g. WiiLink), changes are kept");
    move_cursor(help_row + 2, 0);
    printf("IOS%u = System Menu, asked for separately before it is installed", AX179_SYSMENU_IOS);
    move_cursor(help_row + 4, 0);
    printf("D-Pad: move   [A] select / unselect   [+] all   [-] none     ");
    move_cursor(help_row + 5, 0);
    printf("[1] dry run with the %2d selected IOS   [B] back             ", count_selected());
}

// The list of IOS to patch. Returns true if the dry run should start, false for [B] back.
static bool select_ios_menu(void)
{
    int cursor = 0;
    int count = (int)plat_ios_count;

    clear_screen();
    for (;;) {
        draw_select_menu(cursor);
        u32 pressed = get_button();

        if (pressed == WPAD_BUTTON_UP) {
            if (cursor >= 3)
                cursor = cursor - 3;
        } else if (pressed == WPAD_BUTTON_DOWN) {
            if (cursor + 3 < count)
                cursor = cursor + 3;
        } else if (pressed == WPAD_BUTTON_LEFT) {
            cursor = cursor - 1;
            if (cursor < 0)
                cursor = count - 1;           // jump to the end
        } else if (pressed == WPAD_BUTTON_RIGHT) {
            cursor = cursor + 1;
            if (cursor >= count)
                cursor = 0;                   // jump to the start
        } else if (pressed == WPAD_BUTTON_A) {
            ios_selected[cursor] = !ios_selected[cursor];
        } else if (pressed == WPAD_BUTTON_PLUS) {
            for (int k = 0; k < count; k++)
                ios_selected[k] = true;
        } else if (pressed == WPAD_BUTTON_MINUS) {
            for (int k = 0; k < count; k++)
                ios_selected[k] = false;
        } else if (pressed == WPAD_BUTTON_1) {
            if (count_selected() > 0)
                return true;
        } else if (pressed == WPAD_BUTTON_B || pressed == WPAD_BUTTON_HOME) {
            return false;
        }
    }
}

// The last question before the NAND is written. All backups exist at this point, because the
// dry run was made from them.
static bool confirm_install_179(const u32 *list, int count)
{
    printf("\n=============================================================\n");
    printf(" INSTALL the patched IOS ");
    for (int i = 0; i < count; i++) {
        if (i > 0)
            printf(", ");
        printf("%u", list[i]);
    }
    printf("\n to the %s NAND?\n", plat_name());
    if (plat_is_vwii())
        printf(" Needs a vWii NAND backup (slccmpt.bin + otp.bin).\n");
    else
        printf(" Needs a BootMii NAND backup (nand.bin + keys.bin).\n");
    printf(" Back to the state before patching at any time with [-] Restore.\n");
    printf(" Do NOT power off while it installs.\n");
    printf("=============================================================\n");
    return confirm_sequence();
}

// The System Menu runs on IOS80. So IOS80 gets its own question and can be left out.
static bool confirm_install_sysmenu(void)
{
    printf("\n=============================================================\n");
    printf(" ALSO install the patched IOS%u?\n", AX179_SYSMENU_IOS);
    printf(" The %s System Menu runs on IOS%u. Same driver as IOS36,\n", plat_name(), AX179_SYSMENU_IOS);
    printf(" but if it fails, the System Menu may not start any more.\n");
    if (!plat_is_vwii())
        printf(" On a Wii this is also the IOS of WiiConnect24 in standby.\n");
    printf(" Any other button leaves IOS%u stock.\n", AX179_SYSMENU_IOS);
    printf("=============================================================\n");
    return confirm_sequence();
}

// Asks what to install. Puts the IOS to install into todo[] and returns how many (0 = nothing).
static int ask_what_to_install(const u32 *ready, int ready_count, u32 *todo)
{
    int todo_count = 0;
    bool has_sysmenu_ios = false;

    // IOS80 is taken out of the list, it is asked for on its own
    for (int k = 0; k < ready_count; k++) {
        if (ready[k] == AX179_SYSMENU_IOS) {
            has_sysmenu_ios = true;
        } else {
            todo[todo_count] = ready[k];
            todo_count++;
        }
    }

    if (todo_count > 0) {
        if (!confirm_install_179(todo, todo_count))
            todo_count = 0;               // cancelled
    }
    if (has_sysmenu_ios) {
        if (confirm_install_sysmenu()) {
            todo[todo_count] = AX179_SYSMENU_IOS;
            todo_count++;
        }
    }
    return todo_count;
}

// Step 2: asks before installing, then installs the IOS of the dry run.
static void install_179(const u32 *ready, int ready_count)
{
    u32 todo[MAX_IOS];
    int todo_count = ask_what_to_install(ready, ready_count, todo);

    if (todo_count == 0) {
        printf("\nNot installed.\n");
        return;
    }

    int installed = 0;
    for (int k = 0; k < todo_count; k++) {
        printf("\nInstalling IOS%u - do NOT power off...\n", todo[k]);

        s32 ret = do_patch_179(todo[k], true);
        write_log("install IOS%u -> %d", todo[k], (int)ret);

        if (ret == PATCH_DONE) {
            installed++;
            printf("IOS%u installed.\n", todo[k]);
        } else {
            printf("IOS%u: install FAILED (%d). Do NOT power off yet - note this screen and keep " LOG_FILE ".\n",
                   todo[k], (int)ret);
        }
    }

    if (installed == todo_count) {
        printf("\nDone. Power the console off and on again.\n");
        if (is_ax179a())
            printf("\n%s\n%s\n", AX179A_HINT_1, AX179A_HINT_2);
    } else {
        printf("\n%d of %d IOS installed, %d failed (the failed ones keep their old version).\n",
               installed, todo_count, todo_count - installed);
    }
    write_log("install: %d of %d IOS installed", installed, todo_count);
}

// Step 1: dry run of every selected IOS, then step 2.
static void patch_all_179(void)
{
    int patched = 0;
    int already_done = 0;
    int no_module = 0;
    int no_backup = 0;
    int not_installed = 0;
    int failed = 0;
    u32 ready[MAX_IOS];           // patched in the dry run, can be installed
    u32 failed_list[MAX_IOS];

    clear_screen();
    write_log("patch for %s (product '%s', bcd %04X, %s)", adapter_name(), adapter_product, adapter_bcd, plat_name());

    for (u32 k = 0; k < plat_ios_count; k++) {
        if (!ios_selected[k])
            continue;

        u32 iosnr = plat_ios[k];
        printf("IOS%u:\n", iosnr);

        s32 ret = do_patch_179(iosnr, false);
        write_log("dry run IOS%u -> %d", iosnr, (int)ret);

        if (ret == PATCH_DONE) {
            ready[patched] = iosnr;
            patched++;
        } else if (ret == PATCH_ALREADY_DONE) {
            already_done++;
        } else if (ret == PATCH_NO_MODULE) {
            no_module++;
            printf("  not a supported stock ETH module, skipped\n");
        } else if (ret == PATCH_NO_BACKUP) {
            no_backup++;              // do_patch_179() already printed why
        } else if (ret == PATCH_NOT_INSTALLED) {
            not_installed++;
        } else {
            failed_list[failed] = iosnr;
            failed++;
        }
    }

    printf("\n%d patched, %d already done, %d without supported module, %d without backup, %d not installed, %d failed.\n",
           patched, already_done, no_module, no_backup, not_installed, failed);
    if (failed > 0) {
        printf("Failed: ");
        print_ios_list(failed_list, failed);
        printf("\n");
    }

    if (patched > 0) {
        printf("\nDry run: WADs written to %s/ax179/\n", plat_wad_dir());
        install_179(ready, patched);
    }
    press_any_button();
}


// ===================================================================================
//  8. [A] Patch for the AX88772B / C / D (the way the original tool works)
// ===================================================================================

// Patches one IOS (from the backup on the SD card, or else from the NAND) and installs it.
// Returns 0 patched, 1 nothing to patch (or already done), <0 error.
static s32 patch_772_and_install(u32 iosnr, bool mode_772d)
{
    s32 version = checkIOS(iosnr);
    if (version < 0)
        return -1;

    IOS *ios = NULL;
    s32 ret = get_IOS(&ios, iosnr, (u32)version);
    if (ret < 0)
        return ret;

    // An IOS has either an ARM or a Thumb ETH module, so only one of the two finds something.
    s32 arm_ret;
    s32 thumb_ret;
    if (mode_772d) {
        arm_ret = patch_772d_arm32(ios);
        thumb_ret = patch_772d_thumb(ios);
    } else {
        arm_ret = patch_arm32_ethernet(ios);
        thumb_ret = patch_thumb_ethernet(ios);
    }

    // nothing was patched
    if (arm_ret != 0 && thumb_ret != 0) {
        free_IOS(&ios);
        if (arm_ret < 0 && thumb_ret < 0)
            return -1;
        return 1;
    }

    forge_tmd(ios->tmd);
    encrypt_IOS(ios);
    ret = install_IOS(ios, false);
    free_IOS(&ios);
    return ret;
}

static void print_772_progress(int done, int total)
{
    clear_screen();
    printf("Patching all relevant IOS's... (%d/%d)\n\n", done, total);
}

static void patch_all_772(bool mode_772d)
{
    int total = (int)plat_ios_count;
    int patched = 0;
    int skipped = 0;
    int failed = 0;
    u32 failed_list[MAX_IOS];

    write_log("patch for %s, installs right away", adapter_name());
    print_772_progress(0, total);

    for (u32 i = 0; i < plat_ios_count; i++) {
        s32 ret = patch_772_and_install(plat_ios[i], mode_772d);
        write_log("patch IOS%u -> %d", plat_ios[i], (int)ret);

        if (ret == 0) {
            patched++;
        } else if (ret > 0) {
            skipped++;
        } else {
            failed_list[failed] = plat_ios[i];
            failed++;
        }

        print_772_progress(patched + skipped + failed, total);
    }

    clear_screen();
    if (failed == 0)
        printf("Patching complete!\n");
    else
        printf("Patching finished with %d failures.\n", failed);
    printf("\n%d patched, %d already done, %d failed.\n", patched, skipped, failed);
    if (failed > 0) {
        printf("\nFailed: ");
        print_ios_list(failed_list, failed);
        printf("\n");
    }
    press_any_button();
}

// The first screen of [A]. Returns true if something was patched (then the NAND is read again).
static bool patch_menu(void)
{
    bool mode_179 = is_ax179();
    bool mode_772d = false;
    if (!mode_179 && adapter_pid == PID_772D)
        mode_772d = true;

    clear_screen();
    printf("Patch IOS for %s?\n\n", adapter_name());
    if (mode_179) {
        printf("AX88179 / AX88179A: you choose the IOS on the next screen. Each one is built\n");
        printf("from the backup in %s/ that matches the IOS on the NAND (a WiiLink\n", plat_wad_dir());
        printf("IOS keeps its changes). First a DRY RUN writes WADs to %s/ax179/;\n", plat_wad_dir());
        printf("installing is a separate, confirmed step (IOS%u asked for on its own).\n\n", AX179_SYSMENU_IOS);
        if (is_ax179a())
            printf("%s\n%s\n\n", AX179A_HINT_1, AX179A_HINT_2);
        else
            printf("The driver links at 10/100M (gigabit was unreliable in tests).\n\n");
    }
    printf("Press A to start, anything else to go back.\n");

    if (get_button() != WPAD_BUTTON_A)
        return false;

    if (mode_179) {
        if (!select_ios_menu())
            return false;
        patch_all_179();
    } else {
        patch_all_772(mode_772d);
    }
    return true;
}


// ===================================================================================
//  9. [B] Test network
// ===================================================================================

// After the IOS reload the Wii Remote needs a few seconds to connect again (at most 4 s).
static void wait_for_controller(void)
{
    printf("Waiting for controller...");
    for (int i = 0; i < 40; i++) {
        WPAD_ScanPads();
        u32 type = 0;
        if (WPAD_Probe(0, &type) == WPAD_ERR_NONE)
            break;                    // Wii Remote is there

        PAD_ScanPads();
        if (PAD_ButtonsHeld(0))
            break;                    // a button on a GameCube controller is held

        usleep(100000);
        if (i % 5 == 0)
            printf(".");
    }
    printf("\n");
}

// Opens a connection to api.ipify.org (port 80). Returns the socket, or -1 after printing what failed.
static s32 connect_to_ipify(void)
{
    s32 sock = net_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        printf("(socket failed)\n");
        return -1;
    }

    struct hostent *host = net_gethostbyname("api.ipify.org");
    if (host == NULL || host->h_length <= 0) {
        printf("(DNS failed)\n");
        net_close(sock);
        return -1;
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = 80;       // the Wii is big-endian, so no htons() is needed
    memcpy(&address.sin_addr, host->h_addr_list[0], host->h_length);

    if (net_connect(sock, (struct sockaddr *)&address, sizeof(address)) < 0) {
        printf("(connect failed)\n");
        net_close(sock);
        return -1;
    }
    return sock;
}

// Reads the answer of the server into answer (size = size of that memory) and ends it with a 0.
// Stops when the server closes the connection or sends nothing for 5 seconds.
static void read_answer(s32 sock, char *answer, int size)
{
    int total = 0;
    int idle = 0;       // how many times in a row nothing came (each time 20 ms)

    while (total < size - 1 && idle < 250) {
        int n = net_recv(sock, answer + total, size - 1 - total, 0);
        if (n > 0) {
            total = total + n;
            idle = 0;
        } else if (n == 0) {
            break;                    // the server closed the connection: done
        } else {
            usleep(20000);
            idle++;
        }
    }
    answer[total] = '\0';
}

// The answer is an HTTP header, an empty line, then the IP address.
// Returns the text after the empty line, or NULL if there is none.
static char *find_body(char *answer)
{
    char *body = strstr(answer, "\r\n\r\n");
    if (body != NULL)
        return body + 4;

    body = strstr(answer, "\n\n");
    if (body != NULL)
        return body + 2;

    return NULL;
}

// cuts the text at the first end of line or space
static void cut_after_first_word(char *text)
{
    char *end = text;
    while (*end != '\0' && *end != '\r' && *end != '\n' && *end != ' ')
        end++;
    *end = '\0';
}

// Asks the web page api.ipify.org for our public IP address and prints it.
static void print_public_ip(void)
{
    printf("WAN:   ");

    s32 sock = connect_to_ipify();
    if (sock < 0)
        return;

    const char *request =
        "GET / HTTP/1.0\r\n"
        "Host: api.ipify.org\r\n"
        "Connection: close\r\n\r\n";
    net_send(sock, request, strlen(request), 0);

    char answer[512];
    read_answer(sock, answer, sizeof(answer));
    net_close(sock);

    char *ip = find_body(answer);
    if (ip == NULL) {
        printf("(bad response)\n");
        return;
    }

    cut_after_first_word(ip);
    if (ip[0] == '\0')
        printf("(empty)\n");
    else
        printf("%s\n", ip);
}

// one try: start the network, print the local and the public IP, stop the network again
static void network_try(void)
{
    printf("\nInitializing network");

    // net_init() can need a few tries (up to 10 s)
    s32 net = -1;
    for (int i = 0; i < 50; i++) {
        net = net_init();
        if (net == 0)
            break;
        if (i % 5 == 0)
            printf(".");
        usleep(200000);
    }

    if (net < 0) {
        printf("\nNetwork init failed: %d\n", net);
        printf("Make sure the adapter is plugged in.\n");
        return;
    }

    u32 ip = net_gethostip();
    if (ip == 0) {
        printf("\nNo IP assigned (DHCP timeout?)\n");
    } else {
        int a = (ip >> 24) & 0xFF;
        int b = (ip >> 16) & 0xFF;
        int c = (ip >> 8) & 0xFF;
        int d = ip & 0xFF;
        printf("\nLocal: %d.%d.%d.%d\n", a, b, c, d);
        print_public_ip();
    }
    net_deinit();
}

// The network test runs on IOS58 (the IOS of the Homebrew Channel).
// Afterwards the patcher ends (see main): the network of the IOS keeps running and takes the
// adapter away from the USB list after about a minute.
static void network_test(void)
{
    clear_screen();
    printf("\nReloading IOS58...\n");
    WPAD_Shutdown();
    Patch_AHB();                 // keep AHBPROT after the reload
    USB_Deinitialize();          // otherwise libogc keeps the USB handle of the old IOS
    IOS_ReloadIOS(IOS58_NR);
    USB_Initialize();

    PAD_Init();
    WPAD_Init();
    WPAD_SetDataFormat(WPAD_CHAN_0, WPAD_FMT_BTNS_ACC_IR);
    wait_for_controller();

    for (;;) {
        network_try();

        printf("\nPress B to retry, any other button to leave the patcher.\n");
        if (get_button() != WPAD_BUTTON_B)
            break;
    }
}


// ===================================================================================
//  10. The main screen and main()
// ===================================================================================

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
    if (rmode->viTVMode & VI_NON_INTERLACE)
        VIDEO_WaitVSync();
    CON_InitEx(rmode, 24, 32, rmode->fbWidth - 32, rmode->xfbHeight - 48);
    VIDEO_ClearFrameBuffer(rmode, xfb, COLOR_BLACK);
}

// The rows of the main screen (the first row is row 0)
#define ROW_PATCH_LINE   4     // "[A]     Patch for ..."
#define ROW_CURSOR_PARK  8     // the text cursor waits here, below the menu
#define ROW_HINT_1      10     // the 100M tip for the AX88179A
#define ROW_HINT_2      11
#define ROW_IOS_TITLE   14     // "IOS on NAND ..." and below it the list of all IOS
#define ROW_IOS_LIST    15

// line 4 of the main screen: the [A] entry
static void draw_patch_line(void)
{
    char line[64];

    if (is_patchable())
        snprintf(line, sizeof(line), "[A]     Patch for %s", adapter_name());
    else if (adapter_pid == PID_772A)
        snprintf(line, sizeof(line), "[A]     You don't need to patch, silly!");
    else
        snprintf(line, sizeof(line), "[A]     Please plug in your adapter...");

    move_cursor(ROW_PATCH_LINE, 0);
    printf("%-40s", line);       // "%-40s" fills the text up with spaces to 40 characters
}

// the bottom line of the screen, on the right: "Adapter found! ..."
static void draw_adapter_status(void)
{
    int cols, rows;
    CON_GetMetrics(&cols, &rows);

    char line[64];
    if (is_patchable() || adapter_pid == PID_772A)
        snprintf(line, sizeof(line), "Adapter found! %s.", adapter_name());
    else
        snprintf(line, sizeof(line), "Please plug in your adapter...");

    // clear the whole line first
    move_cursor(rows - 1, 1);
    print_spaces(cols);

    // then print the text right-aligned
    int col = cols - (int)strlen(line);
    if (col < 1)
        col = 1;
    move_cursor(rows - 1, col);
    printf("%s", line);
}

// prints text in a line of 72 characters (filled up with spaces, so an old text is overwritten)
static void print_line_72(const char *text)
{
    printf("%s", text);
    print_spaces(72 - (int)strlen(text));
}

// the 100M tip for the AX88179A (empty lines for other adapters)
static void draw_hint(void)
{
    move_cursor(ROW_HINT_1, 0);
    if (is_ax179a())
        print_line_72(AX179A_HINT_1);
    else
        print_line_72("");

    move_cursor(ROW_HINT_2, 0);
    if (is_ax179a())
        print_line_72(AX179A_HINT_2);
    else
        print_line_72("");
}

// everything on the main screen that changes with the adapter
static void draw_adapter_lines(void)
{
    draw_patch_line();
    draw_adapter_status();
    draw_hint();
    move_cursor(ROW_CURSOR_PARK, 0);
}

// the state of every IOS, 4 per line, 17 columns wide
static void draw_ios_status(void)
{
    move_cursor(ROW_IOS_TITLE, 0);
    printf("IOS on NAND (* = no matching backup in %s/):", plat_wad_dir());

    for (u32 k = 0; k < plat_ios_count; k++) {
        const ios_info *info = &ios_state[k];
        move_cursor(ROW_IOS_LIST + k / 4, (k % 4) * 17);
        printf("IOS%-2u %-8s", plat_ios[k], state_name(info));
        if (backup_missing(info))
            printf("*");
        else
            printf(" ");
    }

    // the explanation of "custom" goes below the last line of the list
    int list_lines = ((int)plat_ios_count + 3) / 4;
    move_cursor(ROW_IOS_LIST + 1 + list_lines, 0);
    printf("custom = not signed by Nintendo (e.g. WiiLink)");
}

static void draw_main_screen(void)
{
    const char *console = "Wii";
    if (plat_is_vwii())
        console = "Wii U (vWii)";

    clear_screen();
    printf("=== vWii Ethernet Patcher " VERSION " ===   console: %s\n\n", console);
    printf("[+]     Dump IOS to %s/ (do this first!)\n", plat_wad_dir());
    printf("[A]     Patch\n");
    printf("[B]     Test network\n");
    printf("[-]     Restore patched IOS from SD\n");
    printf("[HOME]  Exit\n\n");
    draw_ios_status();
    draw_adapter_lines();
}

// Waits for a button on the main screen and returns it.
// Every second the USB bus is checked again, so the screen shows when the adapter is plugged in
// or out. [A] is ignored while no supported adapter is plugged in.
static u32 wait_for_main_button(void)
{
    int ticks = 0;
    u16 last_pid = adapter_pid;
    bool last_ax179 = is_ax179();
    bool last_read_ok = adapter_read_ok;

    for (;;) {
        WPAD_ScanPads();
        u32 pressed = WPAD_ButtonsDown(0) | WPAD_ButtonsDown(1) | WPAD_ButtonsDown(2) | WPAD_ButtonsDown(3);

        if (pressed != 0) {
            bool a_without_adapter = (pressed & WPAD_BUTTON_A) && !is_patchable();
            if (!a_without_adapter)
                return pressed;
        }

        // 20 x 50 ms = 1 second
        usleep(50000);
        ticks++;
        if (ticks < 20)
            continue;
        ticks = 0;

        detect_adapter();

        // only draw again if something changed
        if (adapter_pid != last_pid || is_ax179() != last_ax179 || adapter_read_ok != last_read_ok) {
            last_pid = adapter_pid;
            last_ax179 = is_ax179();
            last_read_ok = adapter_read_ok;
            draw_adapter_lines();
        }
    }
}

// the warning on the first screen
static void show_start_screen(void)
{
    printf("\n\n\n\n\n");
    print_center("This tool modifies IOS modules on your NAND!");
    print_center("I am not responsible for any damage done to your console!");
    printf("\n");
    print_center("Run the IOS dumper before patching!");
    print_center("vWii: Priiloader + Aroma are recommended!");
    print_center("Wii: BootMii (boot2 if possible) + Priiloader are recommended!");
    printf("\n\n");
    print_center("Press any button to continue.");
}

// after a change on the NAND: read the NAND again and look for the adapter again
static void after_nand_change(void)
{
    reinit_nand();
    refresh_status();
    usb_refresh();
}

// Reloads the IOS with hardware access (AHBPROT). Leaves the patcher if that is not possible.
static void get_hardware_access(void)
{
    Patch_AHB();
    bool ahbprot = false;
    if (__IOS_LoadStartupIOS() == 0 && have_ahbprot())
        ahbprot = true;

    PatchIOS(true);
    usleep(1000);
    ISFS_Initialize();

    if (!ahbprot)
        bail("AHBPROT not available. Launch from HBC.");
}

// [B]: runs the network test and then ends the patcher. The network test leaves the network of the
// IOS running, which takes the adapter away from the USB list after about a minute. So the menu could
// not use the adapter any more.
static void test_network_and_leave(void)
{
    network_test();
    if (have_ahbprot()) {
        Patch_AHB();
        reinit_nand();
    }
    printf("\nLeaving the patcher...\n");
}

// Closes everything and returns to the Homebrew Channel.
static void shut_down(void)
{
    Close_SD();
    ISFS_Deinitialize();

    // leave with a freshly loaded IOS58, to be safe (nothing is written after this)
    WPAD_Shutdown();
    USB_Deinitialize();
    IOS_ReloadIOS(IOS58_NR);

    Reboot();
}

int main(int argc, char *argv[])
{
    __exception_setreload(120);   // show a crash screen for 120 s instead of 10 s

    init_video();
    MEM_init();

    show_start_screen();
    WPAD_Init();
    WPAD_SetDataFormat(WPAD_CHAN_0, WPAD_FMT_BTNS_ACC_IR);
    waitforbuttonpress(NULL, NULL);

    get_hardware_access();
    Init_SD();
    plat_init();
    write_log("==== start " VERSION " on %s (%u IOS)", plat_name(), (unsigned)plat_ios_count);

    clear_screen();
    printf("Reading the IOS on the NAND and the backups on the SD card...\n");
    refresh_status();

    // in the AX88179 IOS list every IOS is ticked at the start
    for (int k = 0; k < MAX_IOS; k++)
        ios_selected[k] = true;

    // the main loop: show the main screen, wait for a button, do what the button says
    for (;;) {
        detect_adapter();
        draw_main_screen();
        u32 pressed = wait_for_main_button();

        if (pressed == WPAD_BUTTON_HOME) {
            break;
        } else if (pressed == WPAD_BUTTON_PLUS) {
            dump_all_ios();
            refresh_status();
            usb_refresh();
        } else if (pressed == WPAD_BUTTON_MINUS) {
            restore_stock_ios();
            after_nand_change();
        } else if (pressed == WPAD_BUTTON_A) {
            if (patch_menu())
                after_nand_change();
        } else if (pressed == WPAD_BUTTON_B) {
            test_network_and_leave();
            break;
        }
    }

    shut_down();
    return 0;
}
