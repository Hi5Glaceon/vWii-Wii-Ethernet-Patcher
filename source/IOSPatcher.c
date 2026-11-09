#include <gccore.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>

#include "IOSPatcher.h"
#include "rijndael.h"
#include "sha1.h"
#include "tools.h"
#include "isfs.h"
#include "memory/mem2.hpp"

#define round_up(x,n)	(-(-(x) & -(n)))
#define TITLE_UPPER(x)		( (u32)((x) >> 32) )
#define TITLE_LOWER(x)		((u32)(x))

u8 commonkey[16] = { 0xeb, 0xe4, 0x2a, 0x22, 0x5e, 0x85, 0x93, 0xe4, 0x48, 0xd9, 0xc5, 0x45, 0x73, 0x81, 0xaa, 0xf7 };

s32 Wad_Read_into_memory(char *filename, IOS **ios, u32 iosnr, u32 revision);

void get_title_key(signed_blob *s_tik, u8 *key)
{
	static u8 iv[16] ATTRIBUTE_ALIGN(0x20);
	static u8 keyin[16] ATTRIBUTE_ALIGN(0x20);
	static u8 keyout[16] ATTRIBUTE_ALIGN(0x20);

	const tik *p_tik;
	p_tik = (tik*)SIGNATURE_PAYLOAD(s_tik);
	u8 *enc_key = (u8 *)&p_tik->cipher_title_key;
	memcpy(keyin, enc_key, sizeof keyin);
	memset(keyout, 0, sizeof keyout);
	memset(iv, 0, sizeof iv);
	memcpy(iv, &p_tik->titleid, sizeof p_tik->titleid);

	aes_set_key(commonkey);
	aes_decrypt(iv, keyin, keyout, sizeof keyin);

	memcpy(key, keyout, sizeof keyout);
}

void zero_sig(signed_blob *sig)
{
	u8 *sig_ptr = (u8 *)sig;
	memset(sig_ptr + 4, 0, SIGNATURE_SIZE(sig)-4);
}

s32 brute_tmd(tmd *p_tmd)
{
	u16 fill;
	for(fill=0; fill<65535; fill++)
	{
		p_tmd->fill3=fill;
		sha1 hash;
		SHA1((u8 *)p_tmd, TMD_SIZE(p_tmd), hash);;

		if (hash[0]==0)
		{
			return 0;
		}
	}
	return -1;
}

void forge_tmd(signed_blob *s_tmd)
{
	zero_sig(s_tmd);
	brute_tmd(SIGNATURE_PAYLOAD(s_tmd));
}

void decrypt_buffer(u16 index, u8 *source, u8 *dest, u32 len)
{
	static u8 iv[16];
	memset(iv, 0, 16);
	memcpy(iv, &index, 2);
	aes_decrypt(iv, source, dest, len);
}

void encrypt_buffer(u16 index, u8 *source, u8 *dest, u32 len)
{
	static u8 iv[16];
	memset(iv, 0, 16);
	memcpy(iv, &index, 2);
	aes_encrypt(iv, source, dest, len);
}

void decrypt_IOS(IOS *ios)
{
	u8 key[16];
	get_title_key(ios->ticket, key);
	aes_set_key(key);

	int i;
	for (i = 0; i < ios->content_count; i++)
	{
		decrypt_buffer(i, ios->encrypted_buffer[i], ios->decrypted_buffer[i], ios->buffer_size[i]);
	}
}

void encrypt_IOS(IOS *ios)
{
	u8 key[16];
	get_title_key(ios->ticket, key);
	aes_set_key(key);

	int i;
	for (i = 0; i < ios->content_count; i++)
	{
		encrypt_buffer(i, ios->decrypted_buffer[i], ios->encrypted_buffer[i], ios->buffer_size[i]);
	}
}

s32 Init_IOS(IOS **ios)
{
	if (ios == NULL)
		return -1;

	*ios = MEM2_memalign(32, sizeof(IOS));
	if (*ios == NULL)
		return -1;

	(*ios)->content_count = 0;

	(*ios)->certs = NULL;
	(*ios)->certs_size = 0;
	(*ios)->ticket = NULL;
	(*ios)->ticket_size = 0;
	(*ios)->tmd = NULL;
	(*ios)->tmd_size = 0;
	(*ios)->crl = NULL;
	(*ios)->crl_size = 0;

	(*ios)->encrypted_buffer = NULL;
	(*ios)->decrypted_buffer = NULL;
	(*ios)->buffer_size = NULL;

	return 0;
}

void free_IOS(IOS **ios)
{
	if (ios && *ios)
	{
		if ((*ios)->certs) free((*ios)->certs);
		if ((*ios)->ticket) free((*ios)->ticket);
		if ((*ios)->tmd) free((*ios)->tmd);
		if ((*ios)->crl) free((*ios)->crl);

		int i;
		for (i = 0; i < (*ios)->content_count; i++)
		{
			if ((*ios)->encrypted_buffer && (*ios)->encrypted_buffer[i]) free((*ios)->encrypted_buffer[i]);
			if ((*ios)->decrypted_buffer && (*ios)->decrypted_buffer[i]) free((*ios)->decrypted_buffer[i]);
		}

		if ((*ios)->encrypted_buffer) free((*ios)->encrypted_buffer);
		if ((*ios)->decrypted_buffer) free((*ios)->decrypted_buffer);
		if ((*ios)->buffer_size) free((*ios)->buffer_size);
		free(*ios);
	}
}

s32 set_content_count(IOS *ios, u32 count)
{
	int i;
	if (ios->content_count > 0)
	{
		for (i = 0; i < ios->content_count; i++)
		{
			if (ios->encrypted_buffer && ios->encrypted_buffer[i]) free(ios->encrypted_buffer[i]);
			if (ios->decrypted_buffer && ios->decrypted_buffer[i]) free(ios->decrypted_buffer[i]);
		}

		if (ios->encrypted_buffer) free(ios->encrypted_buffer);
		if (ios->decrypted_buffer) free(ios->decrypted_buffer);
		if (ios->buffer_size) free(ios->buffer_size);
	}

	ios->content_count = count;
	if (count > 0)
	{
		ios->encrypted_buffer = MEM2_memalign(32, 4*count);
		ios->decrypted_buffer = MEM2_memalign(32, 4*count);
		ios->buffer_size = MEM2_memalign(32, 4*count);

		for (i = 0; i < count; i++)
		{
			if (ios->encrypted_buffer) ios->encrypted_buffer[i] = NULL;
			if (ios->decrypted_buffer) ios->decrypted_buffer[i] = NULL;
		}

		if (!ios->encrypted_buffer || !ios->decrypted_buffer || !ios->buffer_size)
		{
			return -1;
		}
	}
	return 0;
}

s32 install_IOS(IOS *ios, bool skipticket)
{
	int ret;
	int cfd;

	if (!skipticket)
	{
		((u8*)(ios->ticket))[0x1F1] = 0x00; // fix ES error -1029
		ret = ES_AddTicket(ios->ticket, ios->ticket_size, ios->certs, ios->certs_size, ios->crl, ios->crl_size);
		if (ret < 0)
		{
			ES_AddTitleCancel();
			return ret;
		}
	}
	ret = ES_AddTitleStart(ios->tmd, ios->tmd_size, ios->certs, ios->certs_size, ios->crl, ios->crl_size);
	if (ret < 0)
	{
		ES_AddTitleCancel();
		return ret;
	}
	tmd *tmd_data  = (tmd *)SIGNATURE_PAYLOAD(ios->tmd);

	int i;
	for (i = 0; i < ios->content_count; i++)
	{
		tmd_content *content = &tmd_data->contents[i];

		cfd = ES_AddContentStart(tmd_data->title_id, content->cid);
		if (cfd < 0)
		{
			ES_AddTitleCancel();
			return cfd;
		}

		ret = ES_AddContentData(cfd, ios->encrypted_buffer[i], ios->buffer_size[i]);
		if (ret < 0)
		{
			ES_AddTitleCancel();
			return ret;
		}

		ret = ES_AddContentFinish(cfd);
		if (ret < 0)
		{
			ES_AddTitleCancel();
			return ret;
		}
	}

	ret = ES_AddTitleFinish();
	if (ret < 0)
	{
		ES_AddTitleCancel();
		return ret;
	}

	return 0;
}

// try sd wad first, then fall back to nand
s32 get_IOS(IOS **ios, u32 iosnr, u32 revision)
{
	char buf[64];
	int ret;
	ret = Init_SD();
	if(ret >= 0)
	{
		sprintf(buf, "sd:/wad/IOS%u-64-v%u.wad", iosnr, revision);
		ret = Wad_Read_into_memory(buf, ios, iosnr, revision);
		if(ret < 0)
		{
			sprintf(buf, "sd:/wad/IOS%u-64-v%u.wad.out.wad", iosnr, revision);
			ret = Wad_Read_into_memory(buf, ios, iosnr, revision);
		}
		Close_SD();
	}
	if(ret < 0)
		ret = Nand_Read_into_memory(ios, iosnr, revision);
	return ret;
}

// get stored TMD from nand
s32 GetTMD(u64 TicketID, signed_blob **Output, u32 *Length)
{
    signed_blob* TMD = NULL;

    u32 TMD_Length;
    s32 ret;

    ret = ES_GetStoredTMDSize(TicketID, &TMD_Length);
    if (ret < 0)
        return ret;

    TMD = (signed_blob*)MEM2_memalign(32, (TMD_Length+31)&(~31));
    if (!TMD)
        return IPC_ENOMEM;

    ret = ES_GetStoredTMD(TicketID, TMD, TMD_Length);
    if (ret < 0)
    {
        free(TMD);
        return ret;
    }

    *Output = TMD;
    *Length = TMD_Length;

    return 0;
}

// returns title version if installed
s32 checkTitle(u64 title_id)
{
	signed_blob *TMD = NULL;
    tmd *t = NULL;
    u32 TMD_size = 0;
    s32 ret = 0;

    ret = GetTMD(title_id, &TMD, &TMD_size);

    if (ret == 0) {
		t = (tmd*)SIGNATURE_PAYLOAD(TMD);
        return t->title_version;
    } else {
		ret = -2;
	}
    free(TMD);
    return ret;
}

s32 checkIOS(u32 IOS)
{
    return checkTitle(((u64)(1) << 32) | (IOS));
}
