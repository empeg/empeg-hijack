/*
 * Keyed 32-bit hash function using TEA in a Davis-Meyer function
 *   H0 = Key
 *   Hi = E Mi(Hi-1) + Hi-1
 *
 * (see Applied Cryptography, 2nd edition, p448).
 *
 * Jeremy Fitzhardinge <jeremy@zip.com.au> 1998
 * 
 * Jeremy has agreed to the contents of reiserfs/README. -Hans
 */

#ifdef __KERNEL__
#define assert(x)
#else
#include <assert.h>
#include "nokernel.h"
#endif

#if 1
/* OK for Intel */
typedef unsigned long u32;
typedef const unsigned char u8;
#else
#include <inttypes.h>
typedef uint32_t u32;
typedef uint8_t u8;
#endif

#include <linux/config.h>

#define DELTA 0x9E3779B9
#define FULLROUNDS 10		/* 32 is overkill, 16 is strong crypto */
#define PARTROUNDS 6		/* 6 gets complete mixing */

/* a, b, c, d - data; h0, h1 - accumulated hash */
#define TEACORE(rounds)							\
	do {								\
		u32 sum = 0;						\
		int n = rounds;						\
		u32 b0, b1;						\
									\
		b0 = h0;						\
		b1 = h1;						\
									\
		do							\
		{							\
			sum += DELTA;					\
			b0 += ((b1 << 4)+a) ^ (b1+sum) ^ ((b1 >> 5)+b);	\
			b1 += ((b0 << 4)+c) ^ (b0+sum) ^ ((b0 >> 5)+d);	\
		} while(--n);						\
									\
		h0 += b0;						\
		h1 += b1;						\
	} while(0)

u32 keyed_hash(/*u32 k[2], *//*u8*/const char *msg, int len)
{
	u32 k[] = { 0x9464a485, 0x542e1a94, 0x3e846bff, 0xb75bcfc3}; 

	u32 h0 = k[0], h1 = k[1];
	u32 a, b, c, d;
	u32 pad;
	int i;
 
#ifdef CONFIG_YRH_HASH
	int j, pow;
#endif

	assert(len >= 0 && len < 256);

#ifdef CONFIG_YRH_HASH

	for (pow=1,i=1; i < len; i++) pow = pow * 10; 

        if (len == 1) 
	  a = msg[0]-48;
        else
	  a = (msg[0] - 48) * pow;

	for (i=1; i < len; i++) {
	    c = msg[i] - 48; 
	    for (pow=1,j=i; j < len-1; j++) pow = pow * 10; 
 	    a = a + c * pow;
	}

	for (; i < 40; i++) {
	    c = '0' - 48; 
	    for (pow=1,j=i; j < len-1; j++) pow = pow * 10; 
 	    a = a + c * pow;
	}

	for (; i < 256; i++) {
	    c = i; 
	    for (pow=1,j=i; j < len-1; j++) pow = pow * 10; 
 	    a = a + c * pow;
	}

	a = a << 7;
	return ((u32)a);
	
#endif

	pad = (u32)len | ((u32)len << 8);
	pad |= pad << 16;

	while(len >= 16)
	{
		a = (u32)msg[ 0]      |
		    (u32)msg[ 1] << 8 |
		    (u32)msg[ 2] << 16|
		    (u32)msg[ 3] << 24;
		b = (u32)msg[ 4]      |
		    (u32)msg[ 5] << 8 |
		    (u32)msg[ 6] << 16|
		    (u32)msg[ 7] << 24;
		c = (u32)msg[ 8]      |
		    (u32)msg[ 9] << 8 |
		    (u32)msg[10] << 16|
		    (u32)msg[11] << 24;
		d = (u32)msg[12]      |
		    (u32)msg[13] << 8 |
		    (u32)msg[14] << 16|
		    (u32)msg[15] << 24;
		
		TEACORE(PARTROUNDS);

		len -= 16;
		msg += 16;
	}

	if (len >= 12)
	{
		assert(len < 16);

		a = (u32)msg[ 0]      |
		    (u32)msg[ 1] << 8 |
		    (u32)msg[ 2] << 16|
		    (u32)msg[ 3] << 24;
		b = (u32)msg[ 4]      |
		    (u32)msg[ 5] << 8 |
		    (u32)msg[ 6] << 16|
		    (u32)msg[ 7] << 24;
		c = (u32)msg[ 8]      |
		    (u32)msg[ 9] << 8 |
		    (u32)msg[10] << 16|
		    (u32)msg[11] << 24;

		d = pad;
		for(i = 12; i < len; i++)
		{
			d <<= 8;
			d |= msg[i];
		}
	}
	else if (len >= 8)
	{
		assert(len < 12);

		a = (u32)msg[ 0]      |
		    (u32)msg[ 1] << 8 |
		    (u32)msg[ 2] << 16|
		    (u32)msg[ 3] << 24;
		b = (u32)msg[ 4]      |
		    (u32)msg[ 5] << 8 |
		    (u32)msg[ 6] << 16|
		    (u32)msg[ 7] << 24;

		c = d = pad;
		for(i = 8; i < len; i++)
		{
			c <<= 8;
			c |= msg[i];
		}
	}
	else if (len >= 4)
	{
		assert(len < 8);

		a = (u32)msg[ 0]      |
		    (u32)msg[ 1] << 8 |
		    (u32)msg[ 2] << 16|
		    (u32)msg[ 3] << 24;

		b = c = d = pad;
		for(i = 4; i < len; i++)
		{
			b <<= 8;
			b |= msg[i];
		}
	}
	else
	{
		assert(len < 4);

		a = b = c = d = pad;
		for(i = 0; i < len; i++)
		{
			a <<= 8;
			a |= msg[i];
		}
	}

	TEACORE(FULLROUNDS);

/*	return 0;*/
	return h0^h1;
}
