#ifndef _IOSPATCHER_H_
#define _IOSPATCHER_H_

#include <gccore.h>

typedef struct {
	signed_blob *certs;
	signed_blob *ticket;
	signed_blob *tmd;
	signed_blob *crl;
	u32 certs_size;
	u32 ticket_size;
	u32 tmd_size;
	u32 crl_size;
	u8 **encrypted_buffer;
	u8 **decrypted_buffer;
	u32 *buffer_size;
	u32 content_count;
} IOS;

void decrypt_IOS(IOS *ios);
void encrypt_IOS(IOS *ios);
s32 Init_IOS(IOS **ios);
void free_IOS(IOS **ios);
s32 set_content_count(IOS *ios, u32 count);
void forge_tmd(signed_blob *s_tmd);
s32 install_IOS(IOS *ios, bool skipticket);
s32 get_IOS(IOS **ios, u32 iosnr, u32 revision);
s32 checkIOS(u32 ios_nr);

#endif
