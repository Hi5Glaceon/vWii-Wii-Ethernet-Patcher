#include <sys/unistd.h>
#include <wiiuse/wpad.h>

void Reboot();
void waitforbuttonpress(u32 *out, u32 *outGC);
s32 Init_SD();
void Close_SD();
#define LOG_FILE "sd:/ethernet_patcher.log"
void write_log(const char *fmt, ...);
