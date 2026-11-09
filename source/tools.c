#include <gccore.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fat.h>
#include <sdcard/wiisd_io.h>
#include "tools.h"

void Reboot()
{
	if (*(u32*)0x80001800) exit(0);
	SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);
}

void waitforbuttonpress(u32 *out, u32 *outGC)
{
	u32 pressed = 0;
	u32 pressedGC = 0;

	while (true)
	{
		WPAD_ScanPads();
		pressed = WPAD_ButtonsDown(0) | WPAD_ButtonsDown(1) | WPAD_ButtonsDown(2) | WPAD_ButtonsDown(3);

		PAD_ScanPads();
		pressedGC = PAD_ButtonsDown(0) | PAD_ButtonsDown(1) | PAD_ButtonsDown(2) | PAD_ButtonsDown(3);

		if(pressed || pressedGC)
		{
			if (pressedGC)
				usleep(20000);
			if (out) *out = pressed;
			if (outGC) *outGC = pressedGC;
			return;
		}
	}
}

s32 Init_SD()
{
	__io_wiisd.shutdown();
	if(!fatMountSimple("sd", &__io_wiisd))
	{
		printf("sd mount failed.\n");
		return -1;
	}
	return 0;
}

void Close_SD()
{
	fatUnmount("sd");
	__io_wiisd.shutdown();
}
