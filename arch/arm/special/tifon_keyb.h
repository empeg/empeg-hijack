#ifndef TFONKEYB_H
#define TFONKEYB_H

/* The mode the pad is in. This is also used by the touch pad driver */
extern unsigned char tifon_keyboard_mode;
extern unsigned char tifon_shift_status;
extern unsigned char tifon_ctrl_status;

/* the three modes of operation */
#define NUM_MODE   0
#define MOUSE_MODE 2
#define ALPHA_MODE 1

/* status flags for the shift and ctrl keys */
#define ACTIVE   1
#define INACTIVE 0

/* some key definitions */
#define NO_KEY 1
#define YES_KEY 17
#define CTRL_KEY 2
#define RIGHT_KEY 7
#define DELETE_KEY 8
#define DOWN_KEY 9
#define UP_KEY 10
#define SHIFT_KEY 16
#define ESCAPE_KEY 18
#define LEFT_KEY 15
#define RETURN_KEY 23
#define SPACE_KEY 57
#define TAB_KEY 58
#define PGUP_KEY 85
#define PGDN_KEY 84
#define END_KEY 91
#define HOME_KEY 92



#endif
