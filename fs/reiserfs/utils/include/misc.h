/*
 * Copyright 1996-2000 Hans Reiser
 */

/* nothing abount reiserfs here */

void die (char * fmt, ...);
void * getmem (int size);
void freemem (void * p);
void * expandmem (void * p, int size, int by);
int is_mounted (char * device_name);
void check_and_free_mem (void);
char * kdevname (int dev);


#ifdef __alpha__

int set_bit (int nr, void * addr);
int clear_bit (int nr, void * addr);
int test_bit(int nr, const void * addr);
int find_first_zero_bit (const void *vaddr, unsigned size);
int find_next_zero_bit (const void *vaddr, unsigned size, unsigned offset);

#else

#include <asm/bitops.h>

#endif

void print_how_far (__u32 * passed, __u32 total);



/*
int test_and_set_bit (int nr, void * addr);
int test_and_clear_bit (int nr, void * addr);
*/
