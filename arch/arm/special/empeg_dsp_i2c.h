#ifndef EMPEG_DSP_I2C_H
#define EMPEG_DSP_I2C_H	1

/* DSP memory: incomplete list of locations, but enough for now */
#define X_modpntr	0x000
#define X_levn		0x001
#define X_leva		0x002
#define X_mlta		0x006
#define X_mltflim	0x007
#define X_pltd		0x00e
#define X_noisflt	0x01c
#define X_leva_u	0x019
#define X_audioc	0x06a
#define X_stepSize	0x120
#define X_counterX	0x121
#define X_plusmax	0x122
#define X_minmax	0x123
#define Y_mod00		0x800		/* Filtered FM level */
#define Y_p1		0x81d
#define Y_q1		0x81e
#define Y_c1		0x821
#define Y_p12		0x837
#define Y_q12		0x838
#define Y_p13		0x83d
#define Y_q13		0x83e
#define Y_p7		0x840
#define Y_q7		0x841
#define Y_minsmtcn	0x842
#define Y_AMb02		0x845		/* thru 0x853 for AM filter */
#define Y_p2		0x84d
#define Y_q2		0x84e
#define Y_minsmtc	0x84f
#define Y_compry0st_28	0x856
#define Y_p3		0x861
#define Y_q3		0x862
#define Y_E_strnf_str	0x867
#define Y_E_mltp_str	0x868
#define Y_stro		0x869
#define Y_p5		0x86d
#define Y_q5		0x86e
#define Y_E_strnf_rsp	0x872
#define Y_E_mltp_rsp	0x873
#define Y_sdr_d_c	0x874
#define Y_p6		0x876
#define Y_q6		0x877
#define Y_c91		0x87b
#define Y_c61		0x87d
#define Y_EMute		0x887

/* Bass/treble, bank0 */
#define Y_Ctl0		0x8b0
#define Y_Cth0		0x8b1
#define Y_Btl0		0x8b2
#define Y_Bth0		0x8b3
#define Y_At00		0x8b4
#define Y_At10		0x8b5
#define Y_At20		0x8b6
#define Y_KTrt0		0x8b7
#define Y_KTft0		0x8b8
#define Y_KTmid0	0x8b9
#define Y_KTbas0	0x8ba
#define Y_KTtre0	0x8bb

/* Bass/treble, bank1 */
#define Y_Ctl1		0x8be
#define Y_Cth1		0x8bf
#define Y_Btl1		0x8c0
#define Y_Bth1		0x8c1
#define Y_At01		0x8c2
#define Y_At11		0x8c3
#define Y_At21		0x8c4
#define Y_KTrt1		0x8c5
#define Y_KTft1		0x8c6
#define Y_KTmid1	0x8c7
#define Y_KTbas1	0x8c8
#define Y_KTtre1	0x8c9

#define Y_VGA		0x8e0
#define Y_KLCl		0x8e1
#define Y_KLCh		0x8e2
#define Y_KLBl		0x8e3
#define Y_KLBh		0x8e4
#define Y_KLA0l		0x8e5
#define Y_KLA0h		0x8e6
#define Y_KLA2l		0x8e7
#define Y_KLA2h		0x8e8
#define Y_KLtre		0x8e9
#define Y_KLbas		0x8ea
#define Y_KLmid		0x8eb
#define Y_VAT		0x8ec
#define Y_SAM		0x8ed
#define Y_OutSwi	0x8ee
#define Y_SrcScal	0x8f3
#define Y_samCl		0x8f4
#define Y_samCh		0x8f5
#define Y_delta		0x8f6
#define Y_switch	0x8f7
#define Y_louSwi	0x8f9
#define Y_statLou	0x8fa
#define Y_OFFS		0x8fb
#define Y_KPDL		0x8fc
#define Y_KMDL		0x8fd
#define Y_Cllev		0x8fe
#define Y_Ctre		0x8ff
#define Y_EMuteF1	0x90e
#define Y_scalS1_	0x927
#define Y_scalS1	0x928
#define Y_cpyS1		0x92a
#define Y_cpyS1_	0x92b
#define Y_c3sin		0x92e
#define Y_c1sin		0x92f
#define Y_c0sin		0x931
#define Y_c2sin		0x932
#define Y_VLsin		0x933
#define Y_VRsin		0x934
#define Y_IClipAmax	0x935
#define Y_IClipAmin	0x936
#define Y_IcoefAl	0x937
#define Y_IcoefAh	0x938
#define Y_IClipBmax	0x939
#define Y_IClipBmin	0x93a
#define Y_IcoefBL	0x93b
#define Y_IcoefBH	0x93c
#define Y_samDecl	0x93d
#define Y_samDech	0x93e
#define Y_deltaD	0x93f
#define Y_switchD	0x940
#define Y_samAttl	0x941
#define Y_samAtth	0x942
#define Y_deltaA	0x943
#define Y_switchA	0x944
#define Y_iSinusWant	0x946
#define Y_sinusMode	0x947
#define Y_tfnFL		0x948
#define Y_tfnFR		0x949
#define Y_tfnBL		0x94a
#define Y_tfnBR		0x94b
#define Y_BALL0		0x8bc
#define Y_BALR0		0x8bd
#define Y_BALL1		0x8ca
#define Y_BALR1		0x8cb
#define Y_FLcof		0x8ef
#define Y_FRcof		0x8f0
#define Y_RLcof		0x8f1
#define Y_RRcof		0x8f2

/* THIS SHOULDN'T BE IN HERE, IT'S ONLY VISITING!
   <altman@empeg.com> */

/* Some DSP defines */
#define IICD_DSP			0x38

#define IIC_DSP_SEL			0x0FFA
#define IIC_DSP_SEL_RESERVED0		0x0001
#define IIC_DSP_SEL_AUX_FM		0x0002
#define IIC_DSP_SEL_AUX_AM_TAPE		0x0004
#define IIC_DSP_SEL_AUX_CD_TAPE		0x0008
#define IIC_DSP_SEL_RESERVED1		0x0010
#define IIC_DSP_SEL_LEV_AMFM		0x0020
#define IIC_DSP_SEL_LEV_WIDENARROW	0x0040
#define IIC_DSP_SEL_LEV_DEF		0x0080
#define IIC_DSP_SEL_BYPASS_PLL		0x0100
#define IIC_DSP_SEL_DC_OFFSET		0x0200
#define IIC_DSP_SEL_RESERVED2		0x0400
#define IIC_DSP_SEL_ADC_SRC		0x0800
#define IIC_DSP_SEL_NSDEC		0x1000
#define IIC_DSP_SEL_INV_HOST_WS		0x2000
#define IIC_DSP_SEL_RESERVED3		0x4000
#define IIC_DSP_SEL_ADC_BW_SWITCH	0x8000

/*
 * Empeg I2C support
 */

#define IIC_CLOCK			EMPEG_I2CCLOCK
#define IIC_DATAOUT			EMPEG_I2CDATA
#define IIC_DATAIN			EMPEG_I2CDATAIN

typedef struct
{
	int address;
	int data;
} dsp_setup;

int i2c_read(unsigned char device, unsigned short address,
	     unsigned int *data, int count);
int i2c_read1(unsigned char device, unsigned short address,
	      unsigned int *data);

int i2c_write(unsigned char device, unsigned short address,
	      unsigned int *data, unsigned short count);
int i2c_write1(unsigned char device, unsigned short address,
	       unsigned int data);

int dsp_read_yram(unsigned short address, unsigned int *data);
int dsp_read_xram(unsigned short address, unsigned int *data);
int dsp_write(unsigned short address, unsigned int data);
int dsp_patchmulti(dsp_setup *setup, int address, int new_data);
int dsp_writemulti(dsp_setup *setup);

#endif
