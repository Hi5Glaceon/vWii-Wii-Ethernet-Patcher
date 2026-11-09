#include <stdio.h>
#include <string.h>
#include <ogcsys.h>
#include "sha1.h"

#include "wad.h"
#include "IOSPatcher.h"
#include "tools.h"
#include "memory/mem2.hpp"

#define BLOCK_SIZE 2048
#define round_up(x,n)	(-(-(x) & -(n)))

#define TITLE_UPPER(x)		( (u32)((x) >> 32) )
#define TITLE_LOWER(x)		((u32)(x))

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
} ATTRIBUTE_PACKED wadHeader;


s32 __Wad_ReadFile(FILE *fp, void *outbuf, u32 offset, u32 len)
{
	s32 ret;

	fseek(fp, offset, SEEK_SET);
	ret = fread(outbuf, len, 1, fp);
	if (ret < 0)
		return ret;

	return 0;
}

s32 __Wad_ReadAlloc(FILE *fp, void **outbuf, u32 offset, u32 len)
{
	void *buffer = NULL;
	s32   ret;

	buffer = MEM2_memalign(32, len);
	if (!buffer)
		return -1;

	ret = __Wad_ReadFile(fp, buffer, offset, len);
	if (ret < 0) {
		free(buffer);
		return ret;
	}
	DCFlushRange(buffer, len);

	*outbuf = buffer;

	return 0;
}



s32 Wad_Read_into_memory(char *filename, IOS **ios, u32 iosnr, u32 revision)
{
	s32 ret;
	FILE *fp = NULL;
	wadHeader *header  = NULL;

	ret = Init_IOS(ios);
	if (ret < 0)
	{
		goto err;
	}

	fp = fopen(filename, "rb");
	if (!fp)
	{
		ret = -1;
		goto err;
	}

	tmd *tmd_data  = NULL;

	u32 cnt, offset = 0;

	ret = __Wad_ReadAlloc(fp, (void *)&header, offset, sizeof(wadHeader));
	if (ret < 0)
	{
		goto err;
	}
	else
		offset += round_up(header->header_len, 64);

	if (header->certs_len == 0 || header->tik_len == 0 || header->tmd_len == 0) 
	{
		ret = -1;
		goto err;
	}

	(*ios)->certs_size = header->certs_len;
	ret = __Wad_ReadAlloc(fp, (void *)&(*ios)->certs, offset, header->certs_len);
	if (ret < 0)
	{
		goto err;
	}
	else
		offset += round_up(header->certs_len, 64);

	if(!IS_VALID_SIGNATURE((signed_blob *)(*ios)->certs)) 
	{
		ret = -1;
		goto err;
	}

	(*ios)->crl_size = header->crl_len;
	if (header->crl_len) 
	{
		ret = __Wad_ReadAlloc(fp, (void *)&(*ios)->crl, offset, header->crl_len);
		if (ret < 0)
		{
			goto err;
		}
		else
			offset += round_up(header->crl_len, 64);
	} else
	{
		(*ios)->crl = NULL;
	}

	(*ios)->ticket_size = header->tik_len;
	ret = __Wad_ReadAlloc(fp, (void *)&(*ios)->ticket, offset, header->tik_len);
	if (ret < 0)
	{
		goto err;
	}
	else
		offset += round_up(header->tik_len, 64);

	if(!IS_VALID_SIGNATURE((signed_blob *)(*ios)->ticket)) 
	{
		ret = -1;
		goto err;
	}

	(*ios)->tmd_size = header->tmd_len;
	ret = __Wad_ReadAlloc(fp, (void *)&(*ios)->tmd, offset, header->tmd_len);
	if (ret < 0)
	{
		goto err;
	}
	else
		offset += round_up(header->tmd_len, 64);

	if(!IS_VALID_SIGNATURE((signed_blob *)(*ios)->tmd)) 
	{
		ret = -1;
		goto err;
	}

	tmd_data = (tmd *)SIGNATURE_PAYLOAD((*ios)->tmd);

	if (TITLE_UPPER(tmd_data->title_id) != 1 || TITLE_LOWER(tmd_data->title_id) != iosnr)
	{
		ret = -1;
		goto err;
	}

	if (tmd_data->title_version != revision)
	{
		ret = -1;
		goto err;
	}

	ret = set_content_count(*ios, tmd_data->num_contents);
	if (ret < 0)
	{
		goto err;
	}

	for (cnt = 0; cnt < tmd_data->num_contents; cnt++) 
	{
		tmd_content *content = &tmd_data->contents[cnt];

		(*ios)->buffer_size[cnt] = round_up((u32)content->size, 64);

		(*ios)->encrypted_buffer[cnt] = MEM2_memalign(32, (*ios)->buffer_size[cnt]);
		(*ios)->decrypted_buffer[cnt] = MEM2_memalign(32, (*ios)->buffer_size[cnt]);

		if (!(*ios)->encrypted_buffer[cnt] || !(*ios)->decrypted_buffer[cnt])
		{
			ret = -1;
			goto err;
		}
		
		ret = __Wad_ReadFile(fp, (*ios)->encrypted_buffer[cnt], offset, (*ios)->buffer_size[cnt]);
		if (ret < 0)		
		{
			goto err;
		}
			
		offset += (*ios)->buffer_size[cnt];
	}
	decrypt_IOS(*ios);

	tmd_content *p_cr = TMD_CONTENTS(tmd_data);
	sha1 hash;
	int i;

	for (i=0;i < (*ios)->content_count;i++)
	{
		SHA1((*ios)->decrypted_buffer[i], (u32)p_cr[i].size, hash);
		if (memcmp(p_cr[i].hash, hash, sizeof hash) != 0)
		{
			ret = -1;
			goto err;
		}
	}	

	goto out;

err:
	free_IOS(ios);

out:
	if (header) free(header);

	if (fp) fclose(fp);

	return ret;
}



