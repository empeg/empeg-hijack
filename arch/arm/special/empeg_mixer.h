#ifndef EMPEG_MIXER_H
#define EMPEG_MIXER_H

#include <linux/config.h>
#include <linux/types.h>
#include <linux/init.h>

int __init empeg_mixer_init(void);
int empeg_mixer_open(struct inode *inode, struct file *file);
int empeg_switch_mixer_open(struct inode *inode, struct file *file);
void empeg_mixer_eq_apply(void);
int empeg_mixer_get_fm_level_fast(void);
int empeg_mixer_get_fm_level(void);
int empeg_mixer_get_stereo(void);
int empeg_mixer_get_vat(void);
void empeg_mixer_clear_stereo(void);
void empeg_mixer_set_stereo(int on);
int empeg_mixer_get_multipath(void);

#endif
