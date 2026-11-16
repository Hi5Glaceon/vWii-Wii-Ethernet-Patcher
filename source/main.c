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
#include <errno.h>
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
#include "cios.h"

extern void __exception_setreload(int t);

#define VERSION "v2.1-beta"

// the IOS this tool runs on
#define IOS58_NR 58

// size of all IOS lists (20 IOS with an ETH module and up to 4 cIOS, see platform.c)
#define MAX_IOS PLAT_MAX_IOS

// USB IDs of the adapters
#define ASIX_VID 0x0B95
#define PID_772A 0x7720     // the first AX88772, already works with the stock IOS
#define PID_772B 0x772B     // AX88772B and AX88772C
#define PID_772D 0x1790     // AX88772D, AX88179 and AX88179A all use this ID

// The AX88179A chooses its link speed itself, the driver cannot limit it to 100M.
// So the user is told to do that on the switch or router.
#define AX179A_HINT_1 "Tip: limit the switch / router port of this console to 100M."
#define AX179A_HINT_2 "With the AX88179A gigabit links were unreliable in tests."

// results of do_patch_179() and patch_772_and_install()
#define PATCH_DONE           0    // patched (and installed, or written to the SD card)
#define PATCH_ALREADY_DONE   1    // the IOS already has the AX88179 driver
#define PATCH_NO_MODULE      2    // the IOS has no ETH module this tool knows
#define PATCH_NO_BACKUP      3    // no backup on the SD card fits the IOS on the NAND
#define PATCH_NOT_INSTALLED -3    // the IOS is not on the NAND
#define PATCH_NO_WAY_BACK   -4    // the patched IOS could not be restored afterwards, not installed
#define PATCH_WRONG_MODULE  -5    // AX88772D: not the ETH module the patch was made for, not installed
                                  // any other negative number: an error

// results of dump_single_ios()
#define DUMP_STOCK           0    // saved as IOSn-64-vV.wad
#define DUMP_MODIFIED        2    // patched IOS on the NAND, not saved
#define DUMP_CUSTOM          3    // saved as IOSn-64-vV-custom.wad
#define DUMP_CIOS            4    // saved as IOSn-64-vV-cios.wad
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

// prints a list like "IOS31, IOS33, IOS34"; a long list goes on in the next line after a comma,
// never in the middle of a name
static void print_ios_list(const u32 *list, int count)
{
    int cols = 80, rows = 0, col = 0, row = 0;
    CON_GetMetrics(&cols, &rows);

    for (int i = 0; i < count; i++) {
        char item[16];
        snprintf(item, sizeof(item), "IOS%u%s", list[i], (i < count - 1) ? "," : "");

        CON_GetPosition(&col, &row);
        if (col > 0 && col + (int)strlen(item) + 1 >= cols)
            printf("\n");
        else if (i > 0)
            printf(" ");
        printf("%s", item);
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

// Reads the version (bcdDevice) of a device that has its own USB ID (AX88772, AX88772B / C): it tells
// the AX88772B (1.00) from the AX88772C (2.00). The device descriptor can only be read from an
// opened device (USB_GetDeviceDescription wants the handle from USB_OpenDevice, not the device ID).
// Only done once per device, like read_1790_info().
static void read_version(s32 device_id, u16 vid, u16 pid)
{
    if (device_id == read_device_id && adapter_read_ok)
        return;

    read_device_id = -1;
    adapter_read_ok = false;
    adapter_bcd = 0;

    s32 fd = -1;
    if (USB_OpenDevice(device_id, vid, pid, &fd) < 0)
        return;

    usb_devdesc desc ATTRIBUTE_ALIGN(32);
    if (read_descriptor(fd, &desc)) {
        adapter_bcd = desc.bcdDevice;
        adapter_read_ok = true;
        read_device_id = device_id;
    }
    USB_CloseDevice(&fd);
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
        read_version(device.device_id, device.vid, device.pid);
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
        // the AX88772C has version 2.00 (read_version(); the two bytes may come in either order, like the AX88179's 1.00)
        if (adapter_bcd == 0x0002 || adapter_bcd == 0x0200)
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
    int signature;    // SIG_NINTENDO (stock), SIG_FAKE (MODIFIED), SIG_OTHER (custom) or SIG_CIOS (cIOS)
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

// "stock", "MODIFIED", "custom", "cIOS" or "missing"
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
        info->signature = ios_sig_class(iosnr, tmd_data, len);

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

// true if the IOS list of this console has a cIOS (then the screens explain the word "cIOS")
static bool list_has_cios(void)
{
    for (u32 k = 0; k < plat_ios_count; k++) {
        if (cios_is_slot(plat_ios[k]))
            return true;
    }
    return false;
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
static bool write_padded(FILE *fp, const void *data, u32 len)
{
    static const u8 zeros[64];

    bool ok = fwrite(data, 1, len, fp) == len;

    u32 pad = round_up_64(len) - len;
    if (pad > 0 && fwrite(zeros, 1, pad, fp) != pad)
        ok = false;
    return ok;
}

// writes an IOS (already encrypted) as a WAD file. 0 ok, -1 the file could not be created,
// -2 writing failed (SD card full or a write error): the incomplete file is deleted again, so a
// cut-off file is never taken for a backup
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

    bool ok = write_padded(fp, &header, sizeof(header));
    ok = write_padded(fp, ios->certs, ios->certs_size) && ok;
    if (ios->crl != NULL && ios->crl_size > 0)
        ok = write_padded(fp, ios->crl, ios->crl_size) && ok;
    ok = write_padded(fp, ios->ticket, ios->ticket_size) && ok;
    ok = write_padded(fp, ios->tmd, ios->tmd_size) && ok;

    // the encrypted buffers are already a multiple of 64 bytes long
    for (int i = 0; i < ios->content_count; i++) {
        u32 size = round_up_64(ios->buffer_size[i]);
        if (fwrite(ios->encrypted_buffer[i], 1, size, fp) != size)
            ok = false;
    }

    // fclose() writes what is still in the buffer, so a full card can show up only here
    if (fclose(fp) != 0)
        ok = false;

    if (!ok) {
        remove(path);
        write_log("writing %s failed (SD card full?), file deleted", path);
        return -2;
    }
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
//   - a cIOS of a USB loader goes to IOSn-64-vV-cios.wad
//   - a MODIFIED IOS (patched) is never saved, so a good backup is never replaced by a patched one
// Returns DUMP_STOCK, DUMP_CUSTOM, DUMP_CIOS, DUMP_MODIFIED or DUMP_ERROR.
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

    int signature = ios_sig_class(iosnr, ios->tmd, ios->tmd_size);
    if (signature == SIG_FAKE) {
        free_IOS(&ios);
        return DUMP_MODIFIED;
    }

    int kind = BK_CUSTOM;
    if (signature == SIG_NINTENDO)
        kind = BK_STOCK;
    else if (signature == SIG_CIOS)
        kind = BK_CIOS;

    char path[96];
    backup_path(path, sizeof(path), kind, iosnr, (u32)version);

    encrypt_IOS(ios);
    ret = write_wad_file(path, ios);
    free_IOS(&ios);

    if (ret < 0)
        return DUMP_ERROR;
    if (kind == BK_STOCK)
        return DUMP_STOCK;
    if (kind == BK_CIOS)
        return DUMP_CIOS;
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
    int cios = count_results(results, total, DUMP_CIOS);
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
    if (cios > 0)
        printf("%d dumped (%d stock, %d custom, %d cIOS).", stock + custom + cios, stock, custom, cios);
    else
        printf("%d dumped (%d stock, %d custom).", stock + custom, stock, custom);

    print_dump_list("Custom (not signed by Nintendo, e.g. WiiLink), saved as *-custom.wad: ", results, DUMP_CUSTOM);
    print_dump_list("cIOS (USB loader), saved as *-cios.wad: ", results, DUMP_CIOS);
    print_dump_list("Patched on NAND, NOT dumped (a dump now would be no backup): ", results, DUMP_MODIFIED);

    // a patched IOS without any backup cannot be brought back from the SD card: say where to look
    for (int i = 0; i < total; i++) {
        if (results[i] == DUMP_MODIFIED && backup_missing(&ios_state[i])) {
            printf("\nSome of them have no backup: see \"No backup\" in the README.");
            break;
        }
    }
    print_dump_list("Failed: ", results, DUMP_ERROR);
    if (failed > 0)
        printf("\n(details in " LOG_FILE "; is the SD card full?)");
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
//   - the backup it was made from (stock, custom or cIOS), or
//   - if another tool changed more than the ETH module: a stock backup of the same version.
// Fills jobs[] and returns how many were found. *missing = MODIFIED IOS without any backup,
// *copies = how many of them only have a patched copy on the SD card (a dump made after patching).
static int find_restore_backups(restore_job *jobs, int *missing, int *copies)
{
    int count = 0;
    *missing = 0;
    *copies = 0;

    for (u32 i = 0; i < plat_ios_count; i++) {
        u32 iosnr = plat_ios[i];

        u32 len;
        signed_blob *tmd_data = read_nand_tmd(iosnr, &len);
        if (tmd_data == NULL)
            continue;                 // not installed

        u32 version = tmd_version(tmd_data);
        if (ios_sig_class(iosnr, tmd_data, len) != SIG_FAKE) {
            free(tmd_data);
            continue;                 // stock, custom or a cIOS nobody patched: nothing to restore
        }

        backup_match match;
        char copy[80];
        bool found = false;
        if (backup_find(iosnr, version, tmd_data, &match) == 0) {
            printf(" IOS%-3u v%-5u -> %s backup (state before patching)\n",
                   iosnr, (unsigned)version, backup_kind_name(match.kind));
            found = true;
        } else if (backup_find_nintendo(iosnr, version, &match) == 0) {
            printf(" IOS%-3u v%-5u -> stock backup (changed by another tool, FULL stock restore)\n",
                   iosnr, (unsigned)version);
            found = true;
        } else if (backup_find_patched_copy(iosnr, version, copy, sizeof(copy)) == 0) {
            printf(" IOS%-3u v%-5u -> only a PATCHED copy on SD (no backup)\n",
                   iosnr, (unsigned)version);
            write_log("IOS%u: %s is a patched copy, not a backup", iosnr, copy);
            (*missing)++;
            (*copies)++;
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
    int copies;

    clear_screen();
    printf("Restore IOS from %s/\n\n", plat_wad_dir());
    Init_SD();
    int count = find_restore_backups(jobs, &missing, &copies);

    if (count == 0) {
        if (copies > 0)
            printf("\nNothing to restore. A patched copy is no backup: see \"No backup\" in README.\n");
        else if (missing > 0)
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
    if (match->kind == BK_CIOS)
        printf("  base: cIOS backup - the changes of the cIOS are kept\n");
    else if (match->kind == BK_CUSTOM || (match->kind == BK_LEGACY && custom))
        printf("  base: custom backup (not signed by Nintendo) - its changes are kept\n");
    else if (match->kind == BK_LEGACY)
        printf("  base: stock (old sd:/wad/) backup\n");
    else
        printf("  base: stock backup\n");
}

// Writes the patched IOS of the dry run to <wad folder>/ax179/. A custom IOS has "-custom" in its
// name, a cIOS "-cios".
static s32 write_dry_run_wad(IOS *ios, u32 iosnr, u32 version, int base_kind, bool custom)
{
    char path[96];
    const char *suffix = "";
    if (base_kind == BK_CIOS)
        suffix = "-cios";
    else if (custom)
        suffix = "-custom";

    Init_SD();
    make_wad_dirs();
    snprintf(path, sizeof(path), "%s/ax179/IOS%u-64-v%u%s-ax179.wad", plat_wad_dir(), iosnr, version, suffix);
    return write_wad_file(path, ios);
}

// Patches one IOS for the AX88179. The patch is always made from the backup on the SD card that
// fits the IOS on the NAND, never from the NAND itself. (A custom IOS such as the one of WiiLink,
// or a cIOS, keeps its changes, because its own backup is used.)
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
        char copy[80];
        printf("  No backup in %s/ matches IOS%u v%u on the NAND.\n", plat_wad_dir(), iosnr, (unsigned)version);
        if (backup_find_patched_copy(iosnr, version, copy, sizeof(copy)) == 0)
            printf("  %s is a copy of a PATCHED IOS, not a backup.\n", copy);
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
        ret = write_dry_run_wad(ios, iosnr, version, match.kind, custom);

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
    if (list_has_cios()) {
        help_row++;                           // one more line, the rest moves down
        move_cursor(help_row + 2, 0);
        printf("cIOS = IOS of a USB loader, not selected at the start");
    }
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

    // two lines, so the screen does not break the text in the middle of a word
    printf("\n%d patched, %d already done, %d without supported module,\n", patched, already_done, no_module);
    printf("%d without backup, %d not installed, %d failed.\n", no_backup, not_installed, failed);
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
//  8. [A] Patch for the AX88772B / C / D (the patch of the original tool)
// ===================================================================================

// True if [-] Restore could take this IOS back once it is installed. patched_tmd is the TMD that is
// about to be installed (already fakesigned). These are the questions find_restore_backups() asks
// about an IOS on the NAND, asked before the install instead of after it:
//   - is it listed as MODIFIED? In a cIOS slot only a changed ETH module that cios.c knows says so.
//     Without it the patched IOS is listed as an untouched cIOS, and [+] Dump writes it over its
//     own backup.
//   - does a backup on the SD card fit it?
static bool can_be_restored(u32 iosnr, u32 version, const signed_blob *patched_tmd, u32 tmd_size)
{
    if (ios_sig_class(iosnr, patched_tmd, tmd_size) != SIG_FAKE)
        return false;

    backup_match match;
    if (backup_find(iosnr, version, patched_tmd, &match) == 0)
        return true;
    return backup_find_nintendo(iosnr, version, &match) == 0;
}

// The AX88772D patch for an ARM ETH module (patch_772d_arm32()) does not only look for byte patterns
// like the other AX88772 patches. It also writes new code to fixed places of the module (0x0AE0,
// 0x0B7C, 0x5248 and from 0x53C0 on). These are places of the IOS36-type module: the script
// tools/ax179/build_patch36.py checks the same addresses in it. In another ARM module that has the
// same patterns, e.g. the one of an older IOS version, other code is at these places and would be
// overwritten, and the IOS would still be installed.
// Returns the position of the stock IOS36-type module in the content list, -1 if there is none.
static int find_ios36_module(const signed_blob *s_tmd)
{
    const tmd *t = (const tmd *)SIGNATURE_PAYLOAD(s_tmd);

    for (u32 i = 0; i < t->num_contents; i++) {
        if (ax179_is_stock_ios36_eth_hash(t->contents[i].hash))
            return (int)i;
    }
    return -1;
}

// Patches one IOS and installs it. Like the AX88179 patch, it is made from the backup on the SD card
// that fits the IOS on the NAND, never from the NAND itself. So an IOS is only patched when there is
// a way back with [-] Restore. (This also means: an IOS with the AX88179 driver goes back to the
// AX88772 driver, because the patch starts from the backup.)
// Returns 0 patched, 1 nothing to patch, PATCH_NO_BACKUP, PATCH_NOT_INSTALLED, PATCH_NO_WAY_BACK,
// PATCH_WRONG_MODULE or another negative number for an error.
static s32 patch_772_and_install(u32 iosnr, bool mode_772d)
{
    Init_SD();

    // which version is on the NAND?
    u32 len;
    signed_blob *nand_tmd = read_nand_tmd(iosnr, &len);
    if (nand_tmd == NULL)
        return PATCH_NOT_INSTALLED;
    u32 version = tmd_version(nand_tmd);

    // the backup the patch is made from (section 7)
    backup_match match;
    IOS *ios = read_backup(iosnr, version, nand_tmd, &match);
    free(nand_tmd);
    if (ios == NULL)
        return PATCH_NO_BACKUP;
    s32 ret;

    // where the IOS36-type module is, asked now because a patch changes its hash in the TMD
    int ios36_module = find_ios36_module(ios->tmd);

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

    // The ARM patch for the AX88772D worked: then the content it changed has to be the IOS36-type
    // module (see find_ios36_module()). If it was, that content no longer has the stock hash.
    if (mode_772d && arm_ret == 0) {
        const tmd *t = (const tmd *)SIGNATURE_PAYLOAD(ios->tmd);
        if (ios36_module < 0 || ax179_is_stock_ios36_eth_hash(t->contents[ios36_module].hash)) {
            write_log("IOS%u: AX88772D patch changed an ETH module it was not made for, not installed", iosnr);
            free_IOS(&ios);
            return PATCH_WRONG_MODULE;
        }
    }

    forge_tmd(ios->tmd);

    // The two patches above look for byte patterns, not for a module with a known hash (as the
    // AX88179 patch does). So they also patch an ETH module the rest of the tool does not know,
    // e.g. the one of a cIOS with another base. Its backup would not fit any more, and in a cIOS
    // slot the result would even be listed as "cIOS" instead of "MODIFIED". Only install what
    // [-] Restore can take back.
    if (!can_be_restored(iosnr, version, ios->tmd, ios->tmd_size)) {
        write_log("IOS%u: the patched IOS could not be restored afterwards, not installed", iosnr);
        free_IOS(&ios);
        return PATCH_NO_WAY_BACK;
    }

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

// All IOS of the list, the cIOS of USB loaders too (they carry the ETH module of their base IOS).
static void patch_all_772(bool mode_772d)
{
    int total = (int)plat_ios_count;
    int patched = 0;
    int skipped = 0;
    int no_backup = 0;
    int failed = 0;
    u32 no_backup_list[MAX_IOS];
    u32 failed_list[MAX_IOS];

    write_log("patch for %s, installs right away", adapter_name());
    print_772_progress(0, total);

    for (u32 i = 0; i < plat_ios_count; i++) {
        s32 ret = patch_772_and_install(plat_ios[i], mode_772d);
        write_log("patch IOS%u -> %d", plat_ios[i], (int)ret);

        if (ret == 0) {
            patched++;
        } else if (ret == PATCH_NO_BACKUP) {
            no_backup_list[no_backup] = plat_ios[i];
            no_backup++;
        } else if (ret > 0) {
            skipped++;
        } else {
            failed_list[failed] = plat_ios[i];
            failed++;
        }

        print_772_progress(patched + skipped + no_backup + failed, total);
    }

    clear_screen();
    if (failed > 0)
        printf("Patching finished with %d failures.\n", failed);
    else if (no_backup > 0)
        printf("Patching finished, %d IOS not patched (no backup).\n", no_backup);
    else
        printf("Patching complete!\n");
    printf("\n%d patched, %d already done, %d without backup, %d failed.\n", patched, skipped, no_backup, failed);
    if (no_backup > 0) {
        printf("\nNo matching backup in %s/: ", plat_wad_dir());
        print_ios_list(no_backup_list, no_backup);
        printf("\nDump with [+] first. An IOS that is already patched needs a stock backup\n");
        printf("of the same version (see \"No backup\" in the README).\n");
    }
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
    } else {
        printf("AX88772: every IOS is built from the backup in %s/ that matches\n", plat_wad_dir());
        printf("the IOS on the NAND, and installed right away. An IOS without a matching\n");
        printf("backup is not patched: dump with [+] first.\n\n");
        if (mode_772d)
            printf("The AX88772D patch is experimental (the original tool says so, too) and\n"
                   "was not tested by this fork.\n\n");
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

    // The answer comes from the network, so it is not trusted: an answer without an address would
    // be read from a NULL pointer (a crash on the console).
    struct hostent *host = net_gethostbyname("api.ipify.org");
    if (host == NULL || host->h_length <= 0 || host->h_addr_list == NULL || host->h_addr_list[0] == NULL) {
        printf("(DNS failed)\n");
        net_close(sock);
        return -1;
    }

    // An IPv4 address has 4 bytes. Exactly these are copied, whatever length the answer claims:
    // a larger one would be written past the end of address.
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = 80;       // the Wii is big-endian, so no htons() is needed
    memcpy(&address.sin_addr, host->h_addr_list[0], sizeof(address.sin_addr));

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

    // read_answer() gives up when nothing comes for 5 seconds. For that net_recv() has to return
    // at once when there is no data yet. On a blocking socket it waits inside the IOS instead,
    // without a limit, as long as the server neither answers nor closes the connection.
    u32 nonblocking = 1;
    net_ioctl(sock, FIONBIO, &nonblocking);

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

// true if the IOS58 on the NAND was changed by this tool (it then has the driver of one adapter only)
static bool ios58_is_modified(void)
{
    for (u32 k = 0; k < plat_ios_count; k++) {
        if (plat_ios[k] == IOS58_NR)
            return ios_state[k].signature == SIG_FAKE;
    }
    return false;
}

// What the user can do when the network does not start. The adapter was found in the main menu
// (before the IOS reload), so adapter_name() still knows it.
// net is the result of the failed start (0 if the IOS did not answer at all). -ETIMEDOUT means libogc
// waited for the link and an IP address and got none: with an IOS58 that is patched this is first of
// all a cable or DHCP problem, even when it is patched for the right adapter.
static void print_network_hint(s32 net)
{
    if (adapter_pid == 0) {
        printf("No ASIX adapter was found. Plug it in and try again.\n");
    } else if (adapter_pid == PID_772A) {
        printf("Adapter: %s. Check the cable and the router (DHCP).\n", adapter_name());
    } else if (is_patchable()) {
        printf("Adapter: %s.\n", adapter_name());
        if (!ios58_is_modified()) {
            // This adapter never works with an IOS58 that is not patched, whatever the error is.
            // (Without a driver for it the start ends with -ETIMEDOUT too, so this comes first:
            // otherwise the first test before patching would only say "check the cable".)
            printf("IOS58 must be patched for it ([A] in the menu). Check the cable too.\n");
        } else if (net == -ETIMEDOUT) {
            printf("No link or no IP address: check the cable and the router (DHCP).\n");
            printf("If the cable is fine: IOS58 may be patched for another adapter ([A]).\n");
        } else {
            printf("IOS58 is patched, maybe for another adapter. Patch it for this one ([A]).\n");
        }
    } else {
        printf("Adapter: %s - this adapter is not supported (see README).\n", adapter_name());
    }
}

// How long the IOS may stay silent while the network starts. libogc itself waits about 20 s at most
// for the link and an IP address and then reports an error, so a start that is silent for longer is
// stuck inside the IOS (e.g. the ETH module waits for an adapter it does not know).
#ifndef NET_ANSWER_TIMEOUT_MS
#define NET_ANSWER_TIMEOUT_MS 40000
#endif

// How long failed starts are repeated. A start that fails at once is tried again 0.2 s later (50
// tries = 10 s). But when no IP address comes (cable not plugged in, no DHCP server) libogc itself
// only gives up after about 10 s: repeating that 50 times left the screen without any reaction for
// more than 8 minutes. So no new try is started after this time; the user can retry with [B].
#ifndef NET_RETRY_WINDOW_MS
#define NET_RETRY_WINDOW_MS 30000
#endif

// set when the IOS did not answer: the network test cannot go on and the IOS has to be reloaded
static bool network_hung = false;

// One start of the network: returns 0 if it runs and a negative number if it failed.
// *no_answer is set if the IOS did not answer in time. net_init() would wait for ever instead,
// it sleeps until the IOS answers, so a stuck IOS would freeze the patcher.
// *waited_ms is set to the time the start took.
static s32 start_network(bool *no_answer, int *waited_ms)
{
    *no_answer = false;
    *waited_ms = 0;

    s32 ret = net_init_async(NULL, NULL);
    if (ret < 0)
        return ret;

    for (int waited = 0; waited < NET_ANSWER_TIMEOUT_MS; waited += 100) {
        s32 status = net_get_status();      // -EBUSY as long as the start is running
        if (status != -EBUSY)
            return status;
        usleep(100000);
        *waited_ms = waited + 100;
    }
    *no_answer = true;
    return -EBUSY;
}

// one try: start the network, print the local and the public IP, stop the network again
static void network_try(void)
{
    printf("\nInitializing network");

    // a start can fail at first and work a moment later: up to 50 tries, 0.2 s apart,
    // but no new try after NET_RETRY_WINDOW_MS
    s32 net = -1;
    bool no_answer = false;
    int total_ms = 0;            // how long all tries took so far
    for (int i = 0; i < 50; i++) {
        int waited_ms = 0;
        net = start_network(&no_answer, &waited_ms);
        if (net == 0 || no_answer)
            break;

        total_ms += waited_ms + 200;
        if (total_ms >= NET_RETRY_WINDOW_MS)
            break;
        if (i % 5 == 0)
            printf(".");
        usleep(200000);
    }

    if (no_answer) {
        // Do not call net_deinit() here: it waits for the answer that never comes. The IOS is
        // reloaded when the patcher ends, which ends the stuck start too.
        network_hung = true;
        printf("\nThe IOS does not answer (%d s).\n", NET_ANSWER_TIMEOUT_MS / 1000);
        print_network_hint(0);
        write_log("network test: no answer from the IOS (adapter pid %04X bcd %04X product '%s')",
                  adapter_pid, adapter_bcd, adapter_product);
        return;
    }

    if (net < 0) {
        net_deinit();            // stop a start that may still be running, so nothing is left behind
        printf("\nNetwork init failed: %d\n", (int)net);
        print_network_hint(net);
        write_log("network test: init failed %d (adapter pid %04X bcd %04X product '%s')",
                  (int)net, adapter_pid, adapter_bcd, adapter_product);
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
        write_log("network test: ok, local IP %d.%d.%d.%d", a, b, c, d);
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
    write_log("network test on IOS58");

    // Close everything that has a handle in the old IOS. libogc keeps these handles across a
    // reload, and in the new IOS the same numbers belong to other devices: closing or using
    // them later (e.g. when leaving the patcher) hits the wrong device and can freeze the console.
    Close_SD();
    ISFS_Deinitialize();
    WPAD_Shutdown();
    USB_Deinitialize();

    Patch_AHB();                 // keep AHBPROT after the reload
    IOS_ReloadIOS(IOS58_NR);
    USB_Initialize();
    Init_SD();                   // fresh handle in IOS58, for the log

    PAD_Init();
    WPAD_Init();
    WPAD_SetDataFormat(WPAD_CHAN_0, WPAD_FMT_BTNS_ACC_IR);
    wait_for_controller();

    for (;;) {
        network_try();

        if (network_hung) {          // a retry would wait for the same IOS
            printf("\nPress any button to leave the patcher.\n");
            get_button();
            break;
        }
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

    if (is_patchable() && adapter_pid == PID_772D && !is_ax179())
        snprintf(line, sizeof(line), "[A]     Patch for %s (experimental)", adapter_name());
    else if (is_patchable())
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

    // the explanation of "custom" (and "cIOS") goes below the last line of the list
    int list_lines = ((int)plat_ios_count + 3) / 4;
    move_cursor(ROW_IOS_LIST + 1 + list_lines, 0);
    printf("custom = not signed by Nintendo (e.g. WiiLink)");
    if (list_has_cios()) {
        move_cursor(ROW_IOS_LIST + 2 + list_lines, 0);
        printf("cIOS   = IOS of a USB loader (e.g. d2x), its changes are kept");
    }
}

static void draw_main_screen(void)
{
    const char *console = "Wii";
    if (plat_is_vwii())
        console = "Wii U (vWii)";

    // the title says what this console is: "Wii Ethernet Patcher" or "vWii Ethernet Patcher"
    clear_screen();
    printf("=== %s Ethernet Patcher " VERSION " ===   console: %s\n\n", plat_name(), console);
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
// Nothing is written to the NAND after the test, so the NAND is not opened again: shut_down() only
// has to unmount the SD card that network_test() mounted.
static void test_network_and_leave(void)
{
    network_test();
    printf("\nLeaving the patcher...\n");
}

// Closes everything and returns to the Homebrew Channel.
static void shut_down(void)
{
    Close_SD();
    ISFS_Deinitialize();

    // leave with a freshly loaded IOS58, to be safe (nothing is written after this)
    WPAD_Shutdown();
    if (!network_hung)           // after a stuck network start the IOS may not answer USB calls either
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

    // In the AX88179 IOS list every IOS is ticked at the start, except a cIOS: patching one is
    // new in this version, so it is only done when it is ticked by hand.
    for (u32 k = 0; k < plat_ios_count; k++)
        ios_selected[k] = !cios_is_slot(plat_ios[k]);

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
