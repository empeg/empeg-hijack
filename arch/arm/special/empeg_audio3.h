#ifndef EMPEG_AUDIO3_H
#define EMPEG_AUDIO3_H

#include <linux/config.h>
#include <linux/types.h>
#include <linux/init.h>

int __init empeg_audio_init(void);
int empeg_audio_open(struct inode *inode, struct file *file);
void empeg_audio_beep_setup(int rate);

#endif
