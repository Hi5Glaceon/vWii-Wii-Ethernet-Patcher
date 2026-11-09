#include <sys/unistd.h>
#include <wiiuse/wpad.h>

void Reboot();
void waitforbuttonpress(u32 *out, u32 *outGC);
s32 Init_SD();
void Close_SD();
