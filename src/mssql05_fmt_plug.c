/*
 * This software is Copyright © 2010 bartavelle, <bartavelle at bandecon.com>, and it is hereby released to the general public under the following terms:
 * Redistribution and use in source and binary forms, with or without modification, are permitted.
 *
 * Modified by Mathieu Perrin (mathieu at tpfh.org) 09/06
 * Microsoft MS-SQL05 password cracker
 *
 * UTF-8 support and use of intrinsics by magnum 2011, no rights reserved
 *
 */

#include <string.h>

#include "arch.h"

#ifdef SHA1_SSE_PARA
#define MMX_COEF	4
#include "sse-intrinsics.h"
#define NBKEYS	(MMX_COEF * SHA1_SSE_PARA)
#elif MMX_COEF
#define NBKEYS	MMX_COEF
#endif

#include "misc.h"
#include "params.h"
#include "common.h"
#include "formats.h"
#include "options.h"
#include "unicode.h"
#include "sha.h"
#include "johnswap.h"

#define FORMAT_LABEL			"mssql05"
#define FORMAT_NAME			"MS-SQL05"

#ifdef SHA1_N_STR
#define ALGORITHM_NAME			"SSE2i " SHA1_N_STR
#elif defined(MMX_COEF) && MMX_COEF == 4
#define ALGORITHM_NAME			"SSE2 4x"
#elif defined(MMX_COEF) && MMX_COEF == 2
#define ALGORITHM_NAME			"MMX 2x"
#elif defined(MMX_COEF)
#define ALGORITHM_NAME			"?"
#else
#define ALGORITHM_NAME			"32/" ARCH_BITS_STR
#endif

#define BENCHMARK_COMMENT		""
#define BENCHMARK_LENGTH		0

#define PLAINTEXT_LENGTH		25
#define CIPHERTEXT_LENGTH		54

#define BINARY_SIZE			20
#define SALT_SIZE			4

#ifdef MMX_COEF
#define MIN_KEYS_PER_CRYPT		NBKEYS
#define MAX_KEYS_PER_CRYPT		NBKEYS
#define GETPOS(i, index)		( (index&(MMX_COEF-1))*4 + ((i)&(0xffffffff-3))*MMX_COEF + (3-((i)&3)) + (index>>(MMX_COEF>>1))*80*MMX_COEF*4 ) //for endianity conversion

#else
#define MIN_KEYS_PER_CRYPT		1
#define MAX_KEYS_PER_CRYPT		1
#endif

//microsoft unicode ...
#if ARCH_LITTLE_ENDIAN
#define ENDIAN_SHIFT_L
#define ENDIAN_SHIFT_R
#else
#define ENDIAN_SHIFT_L  << 8
#define ENDIAN_SHIFT_R  >> 8
#endif

static struct fmt_tests tests[] = {
	{"0x01004086CEB6BF932BC4151A1AF1F13CD17301D70816A8886908", "toto"},
	{"0x01004086CEB60ED526885801C23B366965586A43D3DEAC6DD3FD", "titi"},
	{"0x0100A607BA7C54A24D17B565C59F1743776A10250F581D482DA8B6D6261460D3F53B279CC6913CE747006A2E3254", "foo",    {"User1"} },
	{"0x01000508513EADDF6DB7DDD270CCA288BF097F2FF69CC2DB74FBB9644D6901764F999BAB9ECB80DE578D92E3F80D", "bar",    {"User2"} },
	{"0x01008408C523CF06DCB237835D701C165E68F9460580132E28ED8BC558D22CEDF8801F4503468A80F9C52A12C0A3", "canard", {"User3"} },
	{"0x0100BF088517935FC9183FE39FDEC77539FD5CB52BA5F5761881E5B9638641A79DBF0F1501647EC941F3355440A2", "lapin",  {"User4"} },
	{NULL}
};

static unsigned char cursalt[SALT_SIZE];

#ifdef MMX_COEF
/* Cygwin would not guarantee the alignment if these were declared static */
#define saved_key mssql05_saved_key
#define crypt_key mssql05_crypt_key
#ifdef _MSC_VER
__declspec(align(16)) unsigned char saved_key[80*4*NBKEYS];
__declspec(align(16)) unsigned char crypt_key[BINARY_SIZE*NBKEYS];
#else
unsigned char saved_key[80*4*NBKEYS] __attribute__ ((aligned(16)));
unsigned char crypt_key[BINARY_SIZE*NBKEYS] __attribute__ ((aligned(16)));
#endif
#else

static unsigned char *saved_key;
static ARCH_WORD_32 crypt_key[BINARY_SIZE / 4];
static int key_length;

#endif

static int valid(char *ciphertext, struct fmt_main *pFmt)
{
	int i;

	if (strlen(ciphertext) != CIPHERTEXT_LENGTH) return 0;
	if(memcmp(ciphertext, "0x0100", 6))
		return 0;
	for (i = 6; i < CIPHERTEXT_LENGTH; i++){
		if (!(  (('0' <= ciphertext[i])&&(ciphertext[i] <= '9')) ||
					(('a' <= ciphertext[i])&&(ciphertext[i] <= 'f'))
					|| (('A' <= ciphertext[i])&&(ciphertext[i] <= 'F'))))
			return 0;
	}
	return 1;
}

// Handle full hashes (old and new in one long string) as well. This means the
// [other] mssql format should be registered before this one. If there are
// old-style hashes we should crack them first using that format, then run
// mssql05 with -ru:nt just like LM -> NT format
static char *prepare(char *split_fields[10], struct fmt_main *pFmt)
{
	if (strlen(split_fields[1]) == CIPHERTEXT_LENGTH)
		return split_fields[1];

	if (!memcmp(split_fields[1], "0x0100", 6) && strlen(split_fields[1]) == 94) {
		char cp[CIPHERTEXT_LENGTH + 1];
		strnzcpy(cp, split_fields[1], CIPHERTEXT_LENGTH + 1);

		if (valid(cp,pFmt)) {
			char *cp2 = str_alloc_copy(cp);
			return cp2;
		}
	}
	return split_fields[1];
}

static void set_salt(void *salt)
{
	memcpy(cursalt, salt, SALT_SIZE);
}

static void * get_salt(char * ciphertext)
{
	static unsigned char *out2;
	int l;

	if (!out2) out2 = mem_alloc_tiny(SALT_SIZE, MEM_ALIGN_WORD);

	for(l=0;l<SALT_SIZE;l++)
	{
		out2[l] = atoi16[ARCH_INDEX(ciphertext[l*2+6])]*16
			+ atoi16[ARCH_INDEX(ciphertext[l*2+7])];
	}

	return out2;
}

static void set_key_CP(char *_key, int index);
static void set_key_utf8(char *_key, int index);

static void init(struct fmt_main *pFmt)
{
#ifdef MMX_COEF
	memset(saved_key, 0, sizeof(saved_key));
#else
	saved_key = mem_alloc_tiny(PLAINTEXT_LENGTH*2 + 1 + SALT_SIZE, MEM_ALIGN_WORD);
#endif
	if (options.utf8) {
		pFmt->methods.set_key = set_key_utf8;
		pFmt->params.plaintext_length = PLAINTEXT_LENGTH * 3;
	}
	else if (options.iso8859_1 || options.ascii) {
		; // do nothing
	}
	else {
		pFmt->methods.set_key = set_key_CP;
	}
}

// ISO-8859-1 to UCS-2, directly into vector key buffer
static void set_key(char *_key, int index)
{
#ifdef MMX_COEF
	const unsigned char *key = (unsigned char*)_key;
	unsigned int *keybuf_word = (unsigned int*)&saved_key[GETPOS(3, index)];
	unsigned int len, temp2;

	len = SALT_SIZE >> 1;
	while((temp2 = *key++)) {
		unsigned int temp;
		if ((temp = *key++))
		{
			*keybuf_word = JOHNSWAP((temp << 16) | temp2);
		}
		else
		{
			*keybuf_word = JOHNSWAP(temp2);
			keybuf_word += MMX_COEF;
			*keybuf_word = (0x80 << 8);
			len++;
			goto key_cleaning;
		}
		len += 2;
		keybuf_word += MMX_COEF;
	}
	keybuf_word += MMX_COEF;
	*keybuf_word = (0x80 << 24);

key_cleaning:
	keybuf_word += MMX_COEF;
	while(*keybuf_word) {
		*keybuf_word = 0;
		keybuf_word += MMX_COEF;
	}

	((unsigned int *)saved_key)[15*MMX_COEF + (index&3) + (index>>2)*80*MMX_COEF] = len << 4;
#else
	UTF8 *s = (UTF8*)_key;
	UTF16 *d = (UTF16*)saved_key;
	for (key_length = 0; s[key_length]; key_length++)
		d[key_length] = s[key_length];
	d[key_length] = 0;
	key_length <<= 1;
#endif
}

// Legacy codepage to UCS-2, directly into vector key buffer
static void set_key_CP(char *_key, int index)
{
#ifdef MMX_COEF
	const unsigned char *key = (unsigned char*)_key;
	unsigned int *keybuf_word = (unsigned int*)&saved_key[GETPOS(3, index)];
	unsigned int len, temp2;

	len = SALT_SIZE >> 1;
	while((temp2 = CP_to_Unicode[*key++])) {
		unsigned int temp;
		if ((temp = CP_to_Unicode[*key++]))
		{
			*keybuf_word = JOHNSWAP((temp << 16) | temp2);
		}
		else
		{
			*keybuf_word = JOHNSWAP(temp2);
			keybuf_word += MMX_COEF;
			*keybuf_word = (0x80 << 8);
			len++;
			goto key_cleaning_enc;
		}
		len += 2;
		keybuf_word += MMX_COEF;
	}
	keybuf_word += MMX_COEF;
	*keybuf_word = (0x80 << 24);

key_cleaning_enc:
	keybuf_word += MMX_COEF;
	while(*keybuf_word) {
		*keybuf_word = 0;
		keybuf_word += MMX_COEF;
	}

	((unsigned int *)saved_key)[15*MMX_COEF + (index&3) + (index>>2)*80*MMX_COEF] = len << 4;
#else
	key_length = enc_to_utf16((UTF16*)saved_key, PLAINTEXT_LENGTH + 1,
	                          (unsigned char*)_key, strlen(_key));
	if (key_length <= 0)
		key_length = strlen16((UTF16*)saved_key);
	key_length <<= 1;
#endif
}

// UTF-8 to UCS-2, directly into vector key buffer
static void set_key_utf8(char *_key, int index)
{
#ifdef MMX_COEF
	const UTF8 *source = (UTF8*)_key;
	unsigned int *keybuf_word = (unsigned int*)&saved_key[GETPOS(3, index)];
	UTF32 chl, chh = 0x80;
	unsigned int len;

	len = SALT_SIZE >> 1;
	while (*source) {
		chl = *source;
		if (chl >= 0xC0) {
			unsigned int extraBytesToRead = opt_trailingBytesUTF8[chl & 0x3f];
			switch (extraBytesToRead) {
			case 2:
				++source;
				if (*source) {
					chl <<= 6;
					chl += *source;
				} else
					return;
			case 1:
				++source;
				if (*source) {
					chl <<= 6;
					chl += *source;
				} else
					return;
			case 0:
				break;
			default:
				return;
			}
			chl -= offsetsFromUTF8[extraBytesToRead];
		}
		source++;
		len++;
		if (*source && len < PLAINTEXT_LENGTH + (SALT_SIZE >> 1)) {
			chh = *source;
			if (chh >= 0xC0) {
				unsigned int extraBytesToRead =
					opt_trailingBytesUTF8[chh & 0x3f];
				switch (extraBytesToRead) {
				case 2:
					++source;
					if (*source) {
						chh <<= 6;
						chh += *source;
					} else
						return;
				case 1:
					++source;
					if (*source) {
						chh <<= 6;
						chh += *source;
					} else
						return;
				case 0:
					break;
				default:
					return;
				}
				chh -= offsetsFromUTF8[extraBytesToRead];
			}
			source++;
			len++;
		} else {
			chh = 0xffff;
			*keybuf_word = JOHNSWAP((chh << 16) | chl);
			keybuf_word += MMX_COEF;
			break;
		}
		*keybuf_word = JOHNSWAP((chh << 16) | chl);
		keybuf_word += MMX_COEF;
	}
	if (chh != 0xffff || len == SALT_SIZE >> 1) {
		*keybuf_word = 0xffffffff;
		keybuf_word += MMX_COEF;
		*keybuf_word = (0x80 << 24);
	} else {
		*keybuf_word = 0xffff8000;
	}
	keybuf_word += MMX_COEF;

	while(*keybuf_word) {
		*keybuf_word = 0;
		keybuf_word += MMX_COEF;
	}

	((unsigned int *)saved_key)[15*MMX_COEF + (index&3) + (index>>2)*80*MMX_COEF] = len << 4;
#else
	key_length = utf8_to_utf16((UTF16*)saved_key, PLAINTEXT_LENGTH + 1,
	                           (unsigned char*)_key, strlen(_key));
	if (key_length <= 0)
		key_length = strlen16((UTF16*)saved_key);

	key_length <<= 1;
#endif
}

static char *get_key(int index) {
#ifdef MMX_COEF
	static UTF16 out[PLAINTEXT_LENGTH + 1];
	unsigned int i,s;

	s = ((((unsigned int *)saved_key)[15*MMX_COEF + (index&3) + (index>>2)*80*MMX_COEF] >> 3) - SALT_SIZE) >> 1;
	for(i=0;i<s;i++) {
		out[i] = saved_key[GETPOS(i<<1, index)] |
			(saved_key[GETPOS((i<<1) + 1, index)] << 8);
	}
	out[i] = 0;
	return (char*)utf16_to_enc(out);
#else
	((UTF16*)saved_key)[key_length>>1] = 0;
	return (char*)utf16_to_enc((UTF16*)saved_key);
#endif
}

static int cmp_all(void *binary, int count) {
#ifdef MMX_COEF
#ifdef SHA1_SSE_PARA
	unsigned int x,y=0;

	for(;y<SHA1_SSE_PARA;y++)
	for(x=0;x<MMX_COEF;x++)
	{
		if( ((unsigned int *)binary)[0] == ((unsigned int *)crypt_key)[x+y*MMX_COEF*5] )
			return 1;
	}
	return 0;
#else
	int i=0;
	while(i< (BINARY_SIZE/4) )
	{
		if (
			( ((unsigned long *)binary)[i] != ((unsigned long *)crypt_key)[i*MMX_COEF])
			&& ( ((unsigned long *)binary)[i] != ((unsigned long *)crypt_key)[i*MMX_COEF+1])
#if (MMX_COEF > 3)
			&& ( ((unsigned long *)binary)[i] != ((unsigned long *)crypt_key)[i*MMX_COEF+2])
			&& ( ((unsigned long *)binary)[i] != ((unsigned long *)crypt_key)[i*MMX_COEF+3])
#endif
		)
			return 0;
		i++;
	}
	return 1;
#endif
#else
	return !memcmp(binary, crypt_key, BINARY_SIZE);
#endif
}

static int cmp_exact(char *source, int count){
  return (1);
}

static int cmp_one(void * binary, int index)
{
#ifdef MMX_COEF
#if SHA1_SSE_PARA
	unsigned int x,y;
	x = index&3;
	y = index/4;

	if( ((unsigned int *)binary)[0] != ((unsigned int *)crypt_key)[x+y*MMX_COEF*5] )
		return 0;
	if( ((unsigned int *)binary)[1] != ((unsigned int *)crypt_key)[x+y*MMX_COEF*5+4] )
		return 0;
	if( ((unsigned int *)binary)[2] != ((unsigned int *)crypt_key)[x+y*MMX_COEF*5+8] )
		return 0;
	if( ((unsigned int *)binary)[3] != ((unsigned int *)crypt_key)[x+y*MMX_COEF*5+12] )
		return 0;
	if( ((unsigned int *)binary)[4] != ((unsigned int *)crypt_key)[x+y*MMX_COEF*5+16] )
		return 0;
	return 1;
#else
	int i = 0;
	for(i=0;i<(BINARY_SIZE/4);i++)
		if ( ((unsigned long *)binary)[i] != ((unsigned long *)crypt_key)[i*MMX_COEF+index] )
			return 0;
	return 1;
#endif
#else
	return cmp_all(binary, index);
#endif
}

static void crypt_all(int count) {
#ifdef MMX_COEF
	unsigned i, index;
	for (index = 0; index < count; ++index)
	{
		unsigned len = ((((unsigned int *)saved_key)[15*MMX_COEF + (index&3) + (index>>2)*80*MMX_COEF]) >> 3) - SALT_SIZE;
		for(i=0;i<SALT_SIZE;i++)
			saved_key[GETPOS((len+i), index)] = cursalt[i];
	}
#ifdef SHA1_SSE_PARA
	SSESHA1body(saved_key, (unsigned int *)crypt_key, NULL, 0);
#else
	shammx_nosizeupdate_nofinalbyteswap( (unsigned char *) crypt_key, (unsigned char *) saved_key, 1);
#endif
#else
	SHA_CTX ctx;
	memcpy(saved_key+key_length, cursalt, SALT_SIZE);
	SHA1_Init( &ctx );
	SHA1_Update( &ctx, saved_key, key_length+SALT_SIZE );
	SHA1_Final( (unsigned char *) crypt_key, &ctx);
#endif

}

static void * binary(char *ciphertext)
{
	static char *realcipher;
	int i;

	if(!realcipher) realcipher = mem_alloc_tiny(BINARY_SIZE, MEM_ALIGN_WORD);

	for(i=0;i<BINARY_SIZE;i++)
	{
		realcipher[i] = atoi16[ARCH_INDEX(ciphertext[i*2+14])]*16 + atoi16[ARCH_INDEX(ciphertext[i*2+15])];
	}
#ifdef MMX_COEF
	alter_endianity((unsigned char *)realcipher, BINARY_SIZE);
#endif
	return (void *)realcipher;
}

static int binary_hash_0(void *binary)
{
	return ((ARCH_WORD_32 *)binary)[0] & 0xF;
}

static int binary_hash_1(void *binary)
{
	return ((ARCH_WORD_32 *)binary)[0] & 0xFF;
}

static int binary_hash_2(void *binary)
{
	return ((ARCH_WORD_32 *)binary)[0] & 0xFFF;
}

static int binary_hash_3(void *binary)
{
	return ((ARCH_WORD_32 *)binary)[0] & 0xFFFF;
}

static int binary_hash_4(void *binary)
{
	return ((ARCH_WORD_32 *)binary)[0] & 0xFFFFF;
}

#ifdef SHA1_SSE_PARA
static int get_hash_0(int index)
{
	unsigned int x,y;
        x = index&3;
        y = index/4;
	return ((ARCH_WORD_32*)crypt_key)[x+y*MMX_COEF*5] & 0xf;
}
static int get_hash_1(int index)
{
	unsigned int x,y;
        x = index&3;
        y = index/4;
	return ((ARCH_WORD_32*)crypt_key)[x+y*MMX_COEF*5] & 0xff;
}
static int get_hash_2(int index)
{
	unsigned int x,y;
        x = index&3;
        y = index/4;
	return ((ARCH_WORD_32*)crypt_key)[x+y*MMX_COEF*5] & 0xfff;
}
static int get_hash_3(int index)
{
	unsigned int x,y;
        x = index&3;
        y = index/4;
	return ((ARCH_WORD_32*)crypt_key)[x+y*MMX_COEF*5] & 0xffff;
}
static int get_hash_4(int index)
{
	unsigned int x,y;
        x = index&3;
        y = index/4;
	return ((ARCH_WORD_32*)crypt_key)[x+y*MMX_COEF*5] & 0xfffff;
}
#else
static int get_hash_0(int index)
{
	return ((ARCH_WORD_32 *)crypt_key)[index] & 0xF;
}

static int get_hash_1(int index)
{
	return ((ARCH_WORD_32 *)crypt_key)[index] & 0xFF;
}

static int get_hash_2(int index)
{
	return ((ARCH_WORD_32 *)crypt_key)[index] & 0xFFF;
}

static int get_hash_3(int index)
{
	return ((ARCH_WORD_32 *)crypt_key)[index] & 0xFFFF;
}

static int get_hash_4(int index)
{
	return ((ARCH_WORD_32 *)crypt_key)[index] & 0xFFFFF;
}
#endif

static int salt_hash(void *salt)
{
	// This gave much better distribution on a huge set I analysed
	return (*((ARCH_WORD_32 *)salt) >> 8) & (SALT_HASH_SIZE - 1);
}

struct fmt_main fmt_mssql05 = {
	{
		FORMAT_LABEL,
		FORMAT_NAME,
		ALGORITHM_NAME,
		BENCHMARK_COMMENT,
		BENCHMARK_LENGTH,
		PLAINTEXT_LENGTH,
		BINARY_SIZE,
		SALT_SIZE,
		MIN_KEYS_PER_CRYPT,
		MAX_KEYS_PER_CRYPT,
		FMT_CASE | FMT_8_BIT | FMT_UNICODE | FMT_UTF8,
		tests
	}, {
		init,
		prepare,
		valid,
		fmt_default_split,
		binary,
		get_salt,
		{
			binary_hash_0,
			binary_hash_1,
			binary_hash_2,
			binary_hash_3,
			binary_hash_4
		},
		salt_hash,
		set_salt,
		set_key,
		get_key,
		fmt_default_clear_keys,
		crypt_all,
		{
			get_hash_0,
			get_hash_1,
			get_hash_2,
			get_hash_3,
			get_hash_4
		},
		cmp_all,
		cmp_one,
		cmp_exact
	}
};