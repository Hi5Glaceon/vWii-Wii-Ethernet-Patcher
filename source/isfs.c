#include <stdio.h>
#include <string.h>
#include <ogcsys.h>
#include "IOSPatcher.h"
#include "isfs.h"
#include "sha1.h"
#include "memory/mem2.hpp"

#define round_up(x,n)	(-(-(x) & -(n)))
#define TITLE_UPPER(x)		((u32)((x) >> 32))
#define TITLE_LOWER(x)		((u32)(x))
#define ALIGN(n, x)			(((x) + (n - 1)) & ~(n - 1))

typedef struct map_entry
{
	char filename[8];
	u8 sha1[20];
} ATTRIBUTE_PACKED map_entry_t;

extern void encrypt_IOS(IOS *ios);

static fstats stats ATTRIBUTE_ALIGN(32);
void *Nand_get_file(const char *nand_file, size_t *size)
{
	*size = 0;
	void *buf = NULL;
	s32 fd = ISFS_Open(nand_file, ISFS_OPEN_READ);
	if(fd >= 0)
	{
		memset(&stats, 0, sizeof(fstats));
		if(ISFS_GetFileStats(fd, &stats) >= 0)
		{
			buf = MEM2_memalign(32, stats.file_length);
			if(buf != NULL)
			{
				*size = stats.file_length;
				ISFS_Read(fd, (char*)buf, *size);
			}
		}
		ISFS_Close(fd);
	}
	if(*size > 0)
		DCFlushRange(buf, *size);
	return buf;
}

static void *Nand_get_from_content(u8 *hash, size_t *size, map_entry_t *cm, u32 elements)
{
	void *buf = NULL;
	char ISFS_Filename[32] ATTRIBUTE_ALIGN(32);
	if(cm == NULL || elements == 0)
		return buf;
	u32 i;
	for(i = 0; i < elements; i++)
	{
		if(memcmp(cm[i].sha1, hash, 20) == 0)
		{
			sprintf(ISFS_Filename, "/shared1/%.8s.app", cm[i].filename);
			buf = Nand_get_file(ISFS_Filename, size);
			break;
		}
	}
    return buf;
}

s32 Nand_Read_into_memory(IOS **ios, u32 iosnr, u32 revision)
{
	u8 hash[20];
	char NandFile[256] ATTRIBUTE_ALIGN(0x20);
	tmd *TMD = NULL;
	tmd_content *TMD_Content = NULL;
	void *tmp_buf = NULL;
	map_entry_t *cm = NULL;
	size_t content_map_size = 0;
	size_t content_map_items = 0;
	size_t content_size = 0;
	u32 count = 0;
	s32 ret = Init_IOS(ios);
	if(ret < 0)
		goto error;
	(*ios)->crl = NULL;
	(*ios)->crl_size = 0;
	sprintf(NandFile, "/title/00000001/%08x/content/title.tmd", iosnr);
	(*ios)->tmd = Nand_get_file(NandFile, &((*ios)->tmd_size));
	if((*ios)->tmd == NULL || (*ios)->tmd_size == 0)
	{
		ret = -1;
		goto error;
	}
	TMD = (tmd*)SIGNATURE_PAYLOAD((*ios)->tmd);
	if(TITLE_UPPER(TMD->title_id) != 1 || TITLE_LOWER(TMD->title_id) != iosnr)
	{
		ret = -1;
		goto error;
	}
	if(TMD->title_version != revision)
	{
		ret = -1;
		goto error;
	}
	sprintf(NandFile, "/ticket/00000001/%08x.tik", iosnr);
	(*ios)->ticket = Nand_get_file(NandFile, &((*ios)->ticket_size));
	if((*ios)->ticket == NULL || (*ios)->ticket_size == 0)
	{
		ret = -1;
		goto error;
	}
	sprintf(NandFile, "/sys/cert.sys");
	(*ios)->certs = Nand_get_file(NandFile, &((*ios)->certs_size));
	if((*ios)->certs == NULL || (*ios)->certs_size == 0)
	{
		ret = -1;
		goto error;
	}
	sprintf(NandFile, "/shared1/content.map");
	cm = (map_entry_t*)Nand_get_file(NandFile, &content_map_size);
	if(cm == NULL || content_map_size == 0)
	{
		ret = -1;
		goto error;
	}
	content_map_items = content_map_size/sizeof(map_entry_t);
	ret = set_content_count(*ios, TMD->num_contents);
	if(ret < 0)
	{
		goto error;
	}
	TMD_Content = TMD_CONTENTS(TMD);
	for(count = 0; count < TMD->num_contents; count++)
	{
		sprintf(NandFile, "/title/00000001/%08x/content/%08x.app", iosnr, TMD_Content[count].cid);
		tmp_buf = Nand_get_file(NandFile, &content_size);
		if(tmp_buf == NULL)
			tmp_buf = Nand_get_from_content(TMD_Content[count].hash, &content_size, cm, content_map_items);
		if(tmp_buf == NULL)
		{
			ret = -1;
			goto error;
		}
		if(content_size < TMD_Content[count].size)
		{
			free(tmp_buf);
			ret = -1;
			goto error;
		}
		(*ios)->buffer_size[count] = round_up(ALIGN(16, (u32)TMD_Content[count].size), 64);
		(*ios)->decrypted_buffer[count] = MEM2_memalign(32, (*ios)->buffer_size[count]);
		if((*ios)->decrypted_buffer[count] == NULL)
		{
			ret = -1;
			goto error;
		}
		memset((*ios)->decrypted_buffer[count], 0, (*ios)->buffer_size[count]);
		memcpy((*ios)->decrypted_buffer[count], tmp_buf, TMD_Content[count].size);
		free(tmp_buf);
		tmp_buf = NULL;
		memset(&hash, 0, sizeof(hash));
		SHA1((*ios)->decrypted_buffer[count], TMD_Content[count].size, hash);
		if(memcmp(TMD_Content[count].hash, hash, sizeof(hash)))
		{
			ret = -1;
			goto error;
		}
		(*ios)->encrypted_buffer[count] = MEM2_memalign(32, (*ios)->buffer_size[count]);
		if((*ios)->encrypted_buffer[count] == NULL)
		{
			ret = -1;
			goto error;
		}
	}
	goto finish;
error:
	free_IOS(ios);
finish:
	if(cm != NULL)
		free(cm);
	return ret;
}
