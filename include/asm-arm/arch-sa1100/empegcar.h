#ifndef _INCLUDE_EMPEGCAR_H
#define _INCLUDE_EMPEGCAR_H 1

/* Where flash is mapped in the empeg's kernel memory map */
#define EMPEG_FLASHBASE			0xd0000000

/* Revision 3 (Sonja) IO definitions */
#define EMPEG_POWERFAIL      (GPIO_GPIO0)  /* IN  Power fail sense           */
#define EMPEG_USBIRQ         (GPIO_GPIO1)  /* IN  USB IRQ                    */
#define EMPEG_USBDRQ         (GPIO_GPIO2)  /* IN  USB DRQ                    */
#define EMPEG_CRYSTALDRQ     (GPIO_GPIO2)  /* IN  Mk2 CS4231 audio in DRQ    */
#define EMPEG_RDSCLOCK       (GPIO_GPIO3)  /* IN  RDS clock                  */
#define EMPEG_IRINPUT_BIT    4
#define EMPEG_IRINPUT        GPIO_GPIO(EMPEG_IRINPUT_BIT)  /* IN  Frontboard consumer IR     */
#define EMPEG_DSP2OUT        (GPIO_GPIO5)  /* IN  7705's DSP_OUT2 pin        */
#define EMPEG_IDE1IRQ        (GPIO_GPIO6)  /* IN  IDE channel 1 irq (actL)   */
#define EMPEG_IDE2IRQ        (GPIO_GPIO7)  /* IN  IDE channel 2 irq (actL)   */
#define EMPEG_ETHERNETIRQ    (GPIO_GPIO7)  /* IN  Mk2 Ethernet               */
#define EMPEG_I2CCLOCK       (GPIO_GPIO8)  /* OUT I2C clock                  */
#define EMPEG_I2CDATA        (GPIO_GPIO9)  /* OUT I2C data (when high pulls  */
                                           /*     data line low)             */
#define EMPEG_RADIODATA      (GPIO_GPIO10) /* OUT Radio data (when high it   */
					   /*     pulls data line low)       */
#define EMPEG_DISPLAYCONTROL (GPIO_GPIO10) /* OUT Mk2 display control line   */
#define EMPEG_I2CDATAIN      (GPIO_GPIO11) /* IN  I2C data input             */
#define EMPEG_RADIOCLOCK     (GPIO_GPIO12) /* OUT Radio clock                */
#define EMPEG_SIRSPEED0      (GPIO_GPIO12) /* OUT Mk2 SIR endec speed sel 0  */
#define EMPEG_RADIOWR        (GPIO_GPIO13) /* OUT Radio write enable         */
#define EMPEG_ACCSENSE       (GPIO_GPIO13) /* IN  Mk2 Car accessory sense    */
#define EMPEG_RADIODATAIN    (GPIO_GPIO14) /* IN  Radio data input           */
#define EMPEG_POWERCONTROL   (GPIO_GPIO14) /* OUT Mk2 Control for power PIC  */
#define EMPEG_DSPPOM         (GPIO_GPIO15) /* OUT DSP power-on-mute          */
#define EMPEG_IDERESET       (GPIO_GPIO16) /* OUT IDE hard reset (actL)      */
#define EMPEG_RDSDATA        (GPIO_GPIO17) /* IN  RDS datastream             */
#define EMPEG_DISPLAYPOWER   (GPIO_GPIO18) /* OUT Frontboard power           */
#define EMPEG_I2SCLOCK       (GPIO_GPIO19) /* IN  I2S clock (2.8224Mhz)      */
#define EMPEG_FLASHWE        (GPIO_GPIO20) /* OUT Flash write enable (actL)  */
#define EMPEG_SERIALDSR      (GPIO_GPIO21) /* IN  Serial DSR                 */
#define EMPEG_SERIALCTS      (GPIO_GPIO22) /* IN  Serial CTS                 */
#define EMPEG_SERIALDTR      (GPIO_GPIO23) /* OUT Serial DTS (also LED)      */
#define EMPEG_SIRSPEED1      (GPIO_GPIO23) /* OUT Mk2 SIR endec speed sel 1  */
#define EMPEG_SERIALRTS      (GPIO_GPIO24) /* OUT Serial RTS                 */
#define EMPEG_SIRSPEED2      (GPIO_GPIO24) /* OUT Mk2 SIR endec speed sel 2  */
#define EMPEG_EXTPOWER       (GPIO_GPIO25) /* IN  External power sense (0=   */
                                           /*     unit is in-car)            */
#define EMPEG_DS1821         (GPIO_GPIO26) /* I/O DS1821 data line           */
#define EMPEG_SERIALDCD      (GPIO_GPIO27) /* IN  Serial DCD                 */

/* ... and IRQ defintions */
#define EMPEG_IRQ_IR         4
#define EMPEG_IRQ_USBIRQ     1
#define EMPEG_IRQ_IDE1       6
#define EMPEG_IRQ_IDE2       7
#define EMPEG_IRQ_POWERFAIL  0

#define EMPEG_IR_MAJOR 	     (242)
#define EMPEG_USB_MAJOR      (243)
#define EMPEG_DISPLAY_MAJOR  (244)
#define EMPEG_AUDIO_MAJOR    (245)
#define EMPEG_STATE_MAJOR    (246)
#define EMPEG_RDS_MAJOR      (248)
#define EMPEG_AUDIOIN_MAJOR  (249)
#define EMPEG_POWER_MAJOR    (250)

#if !defined(__ASSEMBLY__)
extern void audio_emitted_action(void);
extern int audio_get_fm_level(void);
extern int audio_get_stereo(void);
extern int audio_get_multipath(void);
#ifdef CONFIG_EMPEG_STATE
extern void enable_powerfail(int enable);
extern int powerfail_enabled(void);
#else
static inline void enable_powerfail(int enable)
{
	/* It's unused */
	enable = enable;
}
#endif /* CONFIG_EMPEG_STATE */

#ifdef CONFIG_EMPEG_DISPLAY
void empeg_displaypower(int on, int no_sched);
static inline void display_powerfail_action(void)
{
	/* Turn off display */
	empeg_displaypower(0,1);

	/* Turn off scan */
	LCCR0=0;
}
extern void display_powerreturn_action(void);
#endif

static inline int empeg_hardwarerevision(void)
{
	/* Return hardware revision */
	unsigned int *id=(unsigned int*)(EMPEG_FLASHBASE+0x2000);

	return(id[0]);
}

static inline unsigned int get_empeg_id(void)
{
	unsigned int *flash=(unsigned int*)(EMPEG_FLASHBASE+0x2000);
	return flash[1];
}
#endif /* !defined(__ASSEMBLY) */

#endif /* _INCLUDE_EMPEGCAR_H */

