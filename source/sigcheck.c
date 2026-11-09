// TMD signature check: is an IOS stock, MODIFIED or custom?
//
//   stock     the signature is Nintendo's. Only Nintendo can make such a signature.
//   MODIFIED  the signature is all zeros. Every patcher (this one too) does that when it installs
//             a changed IOS ("fakesigning").
//   custom    some other signature that is not Nintendo's, e.g. the WiiLink IOS31 and IOS80.
//
// To check a signature it is "decrypted" with Nintendo's public key (RSA). For a real signature
// the result is a fixed pattern followed by the SHA-1 hash of the TMD. The decryption is
//
//     message = signature ^ 65537  mod  n          (n = Nintendo's public key, 2048 bits)
//
// C has no 2048-bit numbers, so they are stored as 64 pieces of 32 bits ("limbs"), lowest piece
// first. mont_mul() multiplies two of them with a known trick (Montgomery multiplication) that
// avoids a slow division. You do not need to understand the math to follow the patcher: only
// tmd_sig_class() at the end is used by the other files.
//
// The key is the retail CP00000004 key, the TMD signer of every Wii and vWii IOS.

#include <string.h>
#include "sigcheck.h"
#include "sha1.h"

#define NL 64                      // 2048 bit / 32

// generated from the CP00000004 certificate in a Nintendo IOS WAD (retail TMD signer)
// little-endian 32-bit limbs; R = 2^2048
static const u32 cp4_n[64] = {
    0x19d33a99, 0xaca70f81, 0x86936c11, 0x77de3263, 0x57227e9e, 0x199a3ddd,
    0x44dc78e7, 0x928732aa, 0xbeae8dfc, 0x021f331c, 0x213e4696, 0xe2b97da0,
    0x1e36a0dc, 0xc72e50d9, 0x4b326793, 0x186183bc, 0x1b91f6de, 0x3aec4683,
    0xcdacf17b, 0x906b8268, 0xf4cb8fd7, 0xe3ab3645, 0x786b59ca, 0x7e33a9ea,
    0x62faa93a, 0x6494804c, 0x6b2c2e20, 0x8c4863cd, 0x53ba1b0d, 0x40a1948a,
    0xb1a2d137, 0x36f0f386, 0xa1cd03ab, 0xd645fce3, 0xd8f84782, 0xe1ace58d,
    0xb605e001, 0x9e5faaa3, 0x73ca3747, 0x85bbd7b7, 0x29d15561, 0x39917736,
    0x917e947c, 0x631a2e7e, 0x5704e6ca, 0x8ce9fee8, 0x2066cc0a, 0x6e27fcb4,
    0xdc2c2e37, 0xcd206dcc, 0x5c1786e2, 0x70b15692, 0x452a9000, 0x62544755,
    0x0cab7704, 0x48335c5d, 0xb86c108d, 0xde145033, 0x9ca68eac, 0x848a6804,
    0x2e97ef14, 0x5b72f033, 0x2955c329, 0xc16df383,
};
static const u32 cp4_r2[64] = {
    0x4e01926f, 0x0e7dcd6e, 0x4be993b0, 0x8c70c3d4, 0x97549785, 0x043074bc,
    0xf74b2275, 0xad129519, 0x1ac165f8, 0x2aa3b655, 0x2283c6ee, 0x5c45e6f2,
    0xea6977d4, 0x6300cfa7, 0x9ba1eb91, 0x26c333e2, 0x5dc65740, 0x2aa72f7a,
    0x09bfa073, 0xe0dec640, 0x3162f76e, 0x488062f9, 0x05383eb4, 0x92eb99ab,
    0xce230112, 0x8f905b57, 0x818b4248, 0x4cf49553, 0x6fc95590, 0xdb6db170,
    0x31c91656, 0x11bbfd9a, 0x1881f60e, 0xfd0762c0, 0x43ad7047, 0x9df54b26,
    0x962592c5, 0x11564ac5, 0xbd9e4c87, 0x71ef4b80, 0x6b5e5481, 0x65173470,
    0x895d2d6e, 0x577823ec, 0xb97a1a72, 0x0bb012d3, 0xb43a1f0d, 0x9d822f91,
    0x027d5e31, 0x610c6534, 0x3dd4ac37, 0x82384eb3, 0x4c642c62, 0x4cab373a,
    0x4e7eb52b, 0xb728bf37, 0x11b6deee, 0x06cbe5c4, 0xbca23cba, 0xa18be236,
    0x193df4d1, 0x32838288, 0x0d6e445a, 0x071ba04e,
};
static const u32 cp4_n0inv = 0xf6c38657;
// sha1 of the big-endian modulus, checked by the self test: 99569e461c0c33dfc4539395b0aeb269c04c1142

static const char cp4_issuer[] = "Root-CA00000001-CP00000004";

// DigestInfo of SHA-1 (RFC 8017), between the 0xFF padding and the hash
static const u8 sha1_digestinfo[15] = {
    0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2b, 0x0e, 0x03, 0x02, 0x1a, 0x05, 0x00, 0x04, 0x14
};

// true if x < n
static bool less_than_n(const u32 *x)
{
    for (int j = NL - 1; j >= 0; j--) {
        if (x[j] != cp4_n[j])
            return x[j] < cp4_n[j];
    }
    return false;          // x == n
}

// x = x - n
static void subtract_n(u32 *x)
{
    u64 borrow = 0;
    for (int j = 0; j < NL; j++) {
        u64 d = (u64)x[j] - cp4_n[j] - borrow;
        x[j] = (u32)d;
        borrow = (d >> 32) & 1;
    }
}

// Montgomery multiplication: r = a * b / R  mod n   (R = 2^2048)
// a and b must be smaller than n; r may be the same array as a or b.
static void mont_mul(u32 *r, const u32 *a, const u32 *b)
{
    u32 t[NL + 2];          // the running sum, 2 limbs longer than a number
    memset(t, 0, sizeof(t));

    for (int i = 0; i < NL; i++) {
        // t = t + a * b[i]
        u64 c = 0;
        for (int j = 0; j < NL; j++) {
            c += (u64)t[j] + (u64)a[j] * b[i];
            t[j] = (u32)c;
            c >>= 32;
        }
        c += t[NL];
        t[NL] = (u32)c;
        t[NL + 1] = (u32)(c >> 32);

        // add a multiple of n so that the lowest limb becomes 0, then drop that limb
        u32 m = t[0] * cp4_n0inv;
        c = (u64)t[0] + (u64)m * cp4_n[0];
        c >>= 32;
        for (int j = 1; j < NL; j++) {
            c += (u64)t[j] + (u64)m * cp4_n[j];
            t[j - 1] = (u32)c;
            c >>= 32;
        }
        c += t[NL];
        t[NL - 1] = (u32)c;
        t[NL] = t[NL + 1] + (u32)(c >> 32);
    }

    // now t < 2n: subtract n once if t >= n
    if (t[NL] != 0 || !less_than_n(t))
        subtract_n(t);

    memcpy(r, t, NL * sizeof(u32));
}

// reads 4 bytes as one big-endian number
static u32 read_be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

// m = s ^ 65537 mod n. s and m are 256 bytes, big-endian. Returns -1 if s >= n.
static int rsa_public(const u8 *s, u8 *m)
{
    u32 x[NL];
    u32 y[NL];
    u32 one[NL];

    // bytes -> limbs (the last 4 bytes are the lowest limb)
    for (int j = 0; j < NL; j++)
        x[j] = read_be32(s + 256 - 4 * (j + 1));

    if (!less_than_n(x))
        return -1;

    // 65537 = 2^16 + 1, so: square 16 times, then multiply with x once more
    mont_mul(x, x, cp4_r2);              // x * R   (the form mont_mul works with)
    memcpy(y, x, sizeof(y));
    for (int k = 0; k < 16; k++)
        mont_mul(y, y, y);               // x^65536 * R
    mont_mul(y, y, x);                   // x^65537 * R
    memset(one, 0, sizeof(one));
    one[0] = 1;
    mont_mul(y, y, one);                 // x^65537   (back to a normal number)

    // limbs -> bytes
    for (int j = 0; j < NL; j++) {
        u8 *p = m + 256 - 4 * (j + 1);
        p[0] = y[j] >> 24;
        p[1] = y[j] >> 16;
        p[2] = y[j] >> 8;
        p[3] = y[j];
    }
    return 0;
}

static bool all_zero(const u8 *data, int len)
{
    for (int i = 0; i < len; i++) {
        if (data[i] != 0)
            return false;
    }
    return true;
}

// Layout of a signed TMD:
//   0x000  signature type (0x00010001 = RSA-2048)
//   0x004  the signature, 256 bytes
//   0x140  issuer ("Root-CA00000001-CP00000004"), the signed part starts here
//   0x1de  number of contents
//   0x1e4  the contents, 36 bytes each; the signed part ends after the last one
int tmd_sig_class(const void *signed_tmd, u32 len)
{
    const u8 *b = (const u8 *)signed_tmd;

    if (b == NULL || len < 0x1e4)
        return SIG_OTHER;

    if (all_zero(b + 4, 256))
        return SIG_FAKE;                 // fakesigned

    if (read_be32(b) != 0x00010001)
        return SIG_OTHER;                // not RSA-2048
    if (memcmp(b + 0x140, cp4_issuer, sizeof(cp4_issuer)) != 0)
        return SIG_OTHER;                // not signed with the TMD key

    u32 num_contents = ((u32)b[0x1de] << 8) | b[0x1df];
    u32 end = 0x1e4 + 36 * num_contents;
    if (end > len)
        return SIG_OTHER;

    // the hash of the signed part
    u8 hash[20];
    SHA1((u8 *)b + 0x140, end - 0x140, hash);

    // decrypt the signature
    u8 m[256];
    if (rsa_public(b + 4, m) < 0)
        return SIG_OTHER;

    // a real signature decrypts to: 00 01 FF FF ... FF 00 <DigestInfo> <SHA-1 of the TMD>
    int info_pos = 256 - 20 - sizeof(sha1_digestinfo);
    u8 expected[256];
    memset(expected, 0xff, sizeof(expected));
    expected[0] = 0x00;
    expected[1] = 0x01;
    expected[info_pos - 1] = 0x00;
    memcpy(expected + info_pos, sha1_digestinfo, sizeof(sha1_digestinfo));
    memcpy(expected + 256 - 20, hash, 20);

    if (memcmp(m, expected, sizeof(expected)) != 0)
        return SIG_OTHER;
    return SIG_NINTENDO;
}

const char *sig_class_name(int c)
{
    if (c == SIG_NINTENDO)
        return "stock";
    if (c == SIG_FAKE)
        return "MODIFIED";
    return "custom";
}
