#
# Makefile for the kernel character device drivers.
#
# Note! Dependencies are done automagically by 'make dep', which also
# removes any old dependencies. DON'T put your own dependencies here
# unless it's something special (ie not a .c file).
#
# Note 2! The CFLAGS definitions are now inherited from the
# parent makes..
#

# Links to make
LK = console_macros.h pc_keyb.c defkeymap.c mouse.h

L_TARGET	:= special.a
L_OBJS		:= 
LX_OBJS		:= 

# Sort out all the architecture stuff...

KEYB_arc	:= defkeymap-acorn.o
KEYB_a5k	:= defkeymap-acorn.o
KEYB_rpc	:= defkeymap-acorn.o
KEYB_ebsa110	:=
KEYB_footbridge	:= defkeymap.o pc_keyb.o

ifeq ($(CONFIG_SA1100_BRUTUS),y)
  KEYB_sa1100	:= defkeymap-acorn.o keyb_brutus.o
endif
ifeq ($(CONFIG_SA1100_TIFON),y)
  KEYB_sa1100	:= defkeymap-tifon.o tifon_keyb.o
endif
ifeq ($(CONFIG_SA1100_THINCLIENT),y)
  KEYB_sa1100	:= defkeymap-acorn.o
endif

ifeq ($(MACHINE),arc)
SOLD_$(CONFIG_SERIAL)		+= serial6850.o
else
S_$(CONFIG_SERIAL)		+= serial.o
endif
S_$(CONFIG_ATOMWIDE_SERIAL)	+= serial.o
S_$(CONFIG_DUALSP_SERIAL)	+= serial.o

ifeq ($(CONFIG_VT),y)
  ifneq ($(KEYB_$(MACHINE)),)
    L_OBJS += $(KEYB_$(MACHINE))
    LX_OBJS += console.o keyboard.o
  endif
endif

ifdef S_y
  LX_OBJS += $(sort $(S_y))
else
  ifdef S_m
    MX_OBJS += $(sort $(S_m))
  endif
endif

ifdef SOLD_y
  LX_OBJS += $(SOLD_y)
else
  ifdef SOLD_m
    MX_OBJS += $(SOLD_m)
  endif
endif

ifdef CONFIG_SA1100_BRUTUS
  L_OBJS	+= audio-sa1100-mcp.o 
  LX_OBJS	+= mcp_common.o
endif

ifdef CONFIG_SA1100_EMPEG
  #L_OBJS        += audio-sa1100-ssp.o
  ifdef CONFIG_PROC_FS
    L_OBJS	+= empeg_proc.o empeg_therm.o
  endif
endif

ifdef CONFIG_EMPEG_INPUT
  L_OBJS += empeg_input.o empeg_input_fiq.o
endif

ifdef CONFIG_EMPEG_STATE
  L_OBJS	+= empeg_state.o
endif

ifdef CONFIG_EMPEG_DISPLAY
  L_OBJS	+= empeg_display.o hijack.o notify.o
endif

ifdef CONFIG_NET_ETHERNET
  L_OBJS	+= kftpd.o
endif

ifdef CONFIG_EMPEG_DSP
  L_OBJS	+= empeg_dsp.o empeg_dsp_i2c.o empeg_audio3.o empeg_mixer.o
endif

ifdef CONFIG_EMPEG_DAC
  L_OBJS	+= empeg_dac.o
endif

ifdef CONFIG_EMPEG_USB9602
  L_OBJS        += empeg_usb.o empeg_usbdma.o
endif

ifdef CONFIG_EMPEG_USBD12
  L_OBJS	+= empeg_pdiusb12.o
endif

ifdef CONFIG_EMPEG_CS4231
  L_OBJS        += empeg_cs4231a.o empeg_cs4231dma.o
endif

ifdef CONFIG_EMPEG_RDS
  L_OBJS        += empeg_rds.o
endif

ifdef CONFIG_EMPEG_POWER
  L_OBJS        += empeg_power.o
endif

ifdef CONFIG_EMPEG_RAMTEST
  L_OBJS	+= empeg_ramtest.o empeg_ramtest_asm.o
endif

ifdef CONFIG_SA1100_CITYGO
  L_OBJS        += audio-sa1100-ssp.o
endif

ifdef CONFIG_SA1100_TIFON
  L_OBJS	+= audio-sa1100-mcp.o 
  LX_OBJS	+= mcp_common.o
endif

CFLAGS += -I.. -I../../../include/linux
# Common dependencies

all: links first_rule

fastdep: links

include $(TOPDIR)/Rules.make

.PHONY: links
links:
	-@for f in $(LK); do \
		if [ ! -e $$f ]; then \
			echo "ln -s ../../../drivers/char/$$f .";\
			ln -s ../../../drivers/char/$$f .; \
		fi; \
	done

mrproper:
	-@for f in $(LK); do \
		if [ -L $$f ]; then \
			echo $(RM) $$f; \
			$(RM) $$f; \
		elif [ -f $$f ]; then \
			echo not removing $$f; \
		fi; \
	done
	$(RM) conmakehash

%.o: %.S
ifneq ($(CONFIG_BINUTILS_NEW),y)
	$(CC) $(CFLAGS) -D__ASSEMBLY__ -E $< | tr ';$$' '\n#' > ..tmp.$<.s
	$(CC) $(CFLAGS:-pipe=) -c -o $@ ..tmp.$<.s
	$(RM) ..tmp.$<.s
else
	$(CC) $(CFLAGS) -D__ASSEMBLY__ -c -o $@ $<
endif
