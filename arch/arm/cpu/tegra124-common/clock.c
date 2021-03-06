/*
 * (C) Copyright 2013
 * NVIDIA Corporation <www.nvidia.com>
 *
 * SPDX-License-Identifier:     GPL-2.0+
 */

/* Tegra124 Clock control functions */

#include <common.h>
#include <asm/io.h>
#include <asm/arch/clock.h>
#include <asm/arch/sysctr.h>
#include <asm/arch/tegra.h>
#include <asm/arch-tegra/clk_rst.h>
#include <asm/arch-tegra/timer.h>
#include <div64.h>
#include <fdtdec.h>

/*
 * Clock types that we can use as a source. The Tegra124 has muxes for the
 * peripheral clocks, and in most cases there are four options for the clock
 * source. This gives us a clock 'type' and exploits what commonality exists
 * in the device.
 *
 * Letters are obvious, except for T which means CLK_M, and S which means the
 * clock derived from 32KHz. Beware that CLK_M (also called OSC in the
 * datasheet) and PLL_M are different things. The former is the basic
 * clock supplied to the SOC from an external oscillator. The latter is the
 * memory clock PLL.
 *
 * See definitions in clock_id in the header file.
 */
enum clock_type_id {
	CLOCK_TYPE_AXPT,	/* PLL_A, PLL_X, PLL_P, CLK_M */
	CLOCK_TYPE_MCPA,	/* and so on */
	CLOCK_TYPE_MCPT,
	CLOCK_TYPE_PCM,
	CLOCK_TYPE_PCMT,
	CLOCK_TYPE_PDCT,
	CLOCK_TYPE_ACPT,
	CLOCK_TYPE_ASPTE,
	CLOCK_TYPE_PMDACD2T,
	CLOCK_TYPE_PCST,

	CLOCK_TYPE_PC2CC3M,
	CLOCK_TYPE_PC2CC3S_T,
	CLOCK_TYPE_PC2CC3M_T,
	CLOCK_TYPE_PC2CC3M_T16,	/* PC2CC3M_T, but w/16-bit divisor (I2C) */
	CLOCK_TYPE_MC2CC3P_A,
	CLOCK_TYPE_M,
	CLOCK_TYPE_MCPTM2C2C3,
	CLOCK_TYPE_PC2CC3T_S,
	CLOCK_TYPE_AC2CC3P_TS2,

	CLOCK_TYPE_COUNT,
	CLOCK_TYPE_NONE = -1,   /* invalid clock type */
};

enum {
	CLOCK_MAX_MUX   = 8     /* number of source options for each clock */
};

/*
 * Clock source mux for each clock type. This just converts our enum into
 * a list of mux sources for use by the code.
 *
 * Note:
 *  The extra column in each clock source array is used to store the mask
 *  bits in its register for the source.
 */
#define CLK(x) CLOCK_ID_ ## x
static enum clock_id clock_source[CLOCK_TYPE_COUNT][CLOCK_MAX_MUX+1] = {
	{ CLK(AUDIO),	CLK(XCPU),	CLK(PERIPH),	CLK(OSC),
		CLK(NONE),	CLK(NONE),	CLK(NONE),	CLK(NONE),
		MASK_BITS_31_30},
	{ CLK(MEMORY),	CLK(CGENERAL),	CLK(PERIPH),	CLK(AUDIO),
		CLK(NONE),	CLK(NONE),	CLK(NONE),	CLK(NONE),
		MASK_BITS_31_30},
	{ CLK(MEMORY),	CLK(CGENERAL),	CLK(PERIPH),	CLK(OSC),
		CLK(NONE),	CLK(NONE),	CLK(NONE),	CLK(NONE),
		MASK_BITS_31_30},
	{ CLK(PERIPH),	CLK(CGENERAL),	CLK(MEMORY),	CLK(NONE),
		CLK(NONE),	CLK(NONE),	CLK(NONE),	CLK(NONE),
		MASK_BITS_31_30},
	{ CLK(PERIPH),	CLK(CGENERAL),	CLK(MEMORY),	CLK(OSC),
		CLK(NONE),	CLK(NONE),	CLK(NONE),	CLK(NONE),
		MASK_BITS_31_30},
	{ CLK(PERIPH),	CLK(DISPLAY),	CLK(CGENERAL),	CLK(OSC),
		CLK(NONE),	CLK(NONE),	CLK(NONE),	CLK(NONE),
		MASK_BITS_31_30},
	{ CLK(AUDIO),	CLK(CGENERAL),	CLK(PERIPH),	CLK(OSC),
		CLK(NONE),	CLK(NONE),	CLK(NONE),	CLK(NONE),
		MASK_BITS_31_30},
	{ CLK(AUDIO),	CLK(SFROM32KHZ),	CLK(PERIPH),	CLK(OSC),
		CLK(EPCI),	CLK(NONE),	CLK(NONE),	CLK(NONE),
		MASK_BITS_31_29},
	{ CLK(PERIPH),	CLK(MEMORY),	CLK(DISPLAY),	CLK(AUDIO),
		CLK(CGENERAL),	CLK(DISPLAY2),	CLK(OSC),	CLK(NONE),
		MASK_BITS_31_29},
	{ CLK(PERIPH),	CLK(CGENERAL),	CLK(SFROM32KHZ),	CLK(OSC),
		CLK(NONE),	CLK(NONE),	CLK(NONE),	CLK(NONE),
		MASK_BITS_31_28},

	/* Additional clock types on Tegra114+ */
	/* CLOCK_TYPE_PC2CC3M */
	{ CLK(PERIPH),	CLK(CGENERAL2),	CLK(CGENERAL),	CLK(CGENERAL3),
		CLK(MEMORY),	CLK(NONE),	CLK(NONE),	CLK(NONE),
		MASK_BITS_31_29},
	/* CLOCK_TYPE_PC2CC3S_T */
	{ CLK(PERIPH),	CLK(CGENERAL2),	CLK(CGENERAL),	CLK(CGENERAL3),
		CLK(SFROM32KHZ), CLK(NONE),	CLK(OSC),	CLK(NONE),
		MASK_BITS_31_29},
	/* CLOCK_TYPE_PC2CC3M_T */
	{ CLK(PERIPH),	CLK(CGENERAL2),	CLK(CGENERAL),	CLK(CGENERAL3),
		CLK(MEMORY),	CLK(NONE),	CLK(OSC),	CLK(NONE),
		MASK_BITS_31_29},
	/* CLOCK_TYPE_PC2CC3M_T, w/16-bit divisor (I2C) */
	{ CLK(PERIPH),	CLK(CGENERAL2),	CLK(CGENERAL),	CLK(CGENERAL3),
		CLK(MEMORY),	CLK(NONE),	CLK(OSC),	CLK(NONE),
		MASK_BITS_31_29},
	/* CLOCK_TYPE_MC2CC3P_A */
	{ CLK(MEMORY),	CLK(CGENERAL2),	CLK(CGENERAL),	CLK(CGENERAL3),
		CLK(PERIPH),	CLK(NONE),	CLK(AUDIO),	CLK(NONE),
		MASK_BITS_31_29},
	/* CLOCK_TYPE_M */
	{ CLK(MEMORY),		CLK(NONE),	CLK(NONE),	CLK(NONE),
		CLK(NONE),	CLK(NONE),	CLK(NONE),	CLK(NONE),
		MASK_BITS_31_30},
	/* CLOCK_TYPE_MCPTM2C2C3 */
	{ CLK(MEMORY),	CLK(CGENERAL),	CLK(PERIPH),	CLK(OSC),
		CLK(MEMORY2),	CLK(CGENERAL2),	CLK(CGENERAL3),	CLK(NONE),
		MASK_BITS_31_29},
	/* CLOCK_TYPE_PC2CC3T_S */
	{ CLK(PERIPH),	CLK(CGENERAL2),	CLK(CGENERAL),	CLK(CGENERAL3),
		CLK(OSC),	CLK(NONE),	CLK(SFROM32KHZ), CLK(NONE),
		MASK_BITS_31_29},
	/* CLOCK_TYPE_AC2CC3P_TS2 */
	{ CLK(AUDIO),	CLK(CGENERAL2),	CLK(CGENERAL),	CLK(CGENERAL3),
		CLK(PERIPH),	CLK(NONE),	CLK(OSC),	CLK(SRC2),
		MASK_BITS_31_29},
};

/*
 * Clock type for each peripheral clock source. We put the name in each
 * record just so it is easy to match things up
 */
#define TYPE(name, type) type
static enum clock_type_id clock_periph_type[PERIPHC_COUNT] = {
	/* 0x00 */
	TYPE(PERIPHC_I2S1,	CLOCK_TYPE_AXPT),
	TYPE(PERIPHC_I2S2,	CLOCK_TYPE_AXPT),
	TYPE(PERIPHC_SPDIF_OUT,	CLOCK_TYPE_AXPT),
	TYPE(PERIPHC_SPDIF_IN,	CLOCK_TYPE_PC2CC3M),
	TYPE(PERIPHC_PWM,	CLOCK_TYPE_PC2CC3S_T),
	TYPE(PERIPHC_05h,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_SBC2,	CLOCK_TYPE_PC2CC3M_T),
	TYPE(PERIPHC_SBC3,	CLOCK_TYPE_PC2CC3M_T),

	/* 0x08 */
	TYPE(PERIPHC_08h,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_I2C1,	CLOCK_TYPE_PC2CC3M_T16),
	TYPE(PERIPHC_I2C5,	CLOCK_TYPE_PC2CC3M_T16),
	TYPE(PERIPHC_0bh,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_0ch,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_SBC1,	CLOCK_TYPE_PC2CC3M_T),
	TYPE(PERIPHC_DISP1,	CLOCK_TYPE_PMDACD2T),
	TYPE(PERIPHC_DISP2,	CLOCK_TYPE_PMDACD2T),

	/* 0x10 */
	TYPE(PERIPHC_10h,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_11h,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_VI,	CLOCK_TYPE_MC2CC3P_A),
	TYPE(PERIPHC_13h,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_SDMMC1,	CLOCK_TYPE_PC2CC3M_T),
	TYPE(PERIPHC_SDMMC2,	CLOCK_TYPE_PC2CC3M_T),
	TYPE(PERIPHC_16h,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_17h,	CLOCK_TYPE_NONE),

	/* 0x18 */
	TYPE(PERIPHC_18h,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_SDMMC4,	CLOCK_TYPE_PC2CC3M_T),
	TYPE(PERIPHC_VFIR,	CLOCK_TYPE_PC2CC3M_T),
	TYPE(PERIPHC_1Bh,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_1Ch,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_HSI,	CLOCK_TYPE_PC2CC3M_T),
	TYPE(PERIPHC_UART1,	CLOCK_TYPE_PC2CC3M_T),
	TYPE(PERIPHC_UART2,	CLOCK_TYPE_PC2CC3M_T),

	/* 0x20 */
	TYPE(PERIPHC_HOST1X,	CLOCK_TYPE_MC2CC3P_A),
	TYPE(PERIPHC_21h,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_22h,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_HDMI,	CLOCK_TYPE_PMDACD2T),
	TYPE(PERIPHC_24h,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_25h,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_I2C2,	CLOCK_TYPE_PC2CC3M_T16),
	TYPE(PERIPHC_EMC,	CLOCK_TYPE_MCPTM2C2C3),

	/* 0x28 */
	TYPE(PERIPHC_UART3,	CLOCK_TYPE_PC2CC3M_T),
	TYPE(PERIPHC_29h,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_VI_SENSOR,	CLOCK_TYPE_MC2CC3P_A),
	TYPE(PERIPHC_2bh,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_2ch,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_SBC4,	CLOCK_TYPE_PC2CC3M_T),
	TYPE(PERIPHC_I2C3,	CLOCK_TYPE_PC2CC3M_T16),
	TYPE(PERIPHC_SDMMC3,	CLOCK_TYPE_PC2CC3M_T),

	/* 0x30 */
	TYPE(PERIPHC_UART4,	CLOCK_TYPE_PC2CC3M_T),
	TYPE(PERIPHC_UART5,	CLOCK_TYPE_PC2CC3M_T),
	TYPE(PERIPHC_VDE,	CLOCK_TYPE_PC2CC3M_T),
	TYPE(PERIPHC_OWR,	CLOCK_TYPE_PC2CC3M_T),
	TYPE(PERIPHC_NOR,	CLOCK_TYPE_PC2CC3M_T),
	TYPE(PERIPHC_CSITE,	CLOCK_TYPE_PC2CC3M_T),
	TYPE(PERIPHC_I2S0,	CLOCK_TYPE_AXPT),
	TYPE(PERIPHC_DTV,	CLOCK_TYPE_NONE),

	/* 0x38 */
	TYPE(PERIPHC_38h,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_39h,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_3ah,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_3bh,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_MSENC,	CLOCK_TYPE_MC2CC3P_A),
	TYPE(PERIPHC_TSEC,	CLOCK_TYPE_PC2CC3M_T),
	TYPE(PERIPHC_3eh,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_OSC,	CLOCK_TYPE_NONE),

	/* 0x40 */
	TYPE(PERIPHC_40h,	CLOCK_TYPE_NONE),	/* start with 0x3b0 */
	TYPE(PERIPHC_MSELECT,	CLOCK_TYPE_PC2CC3M_T),
	TYPE(PERIPHC_TSENSOR,	CLOCK_TYPE_PC2CC3T_S),
	TYPE(PERIPHC_I2S3,	CLOCK_TYPE_AXPT),
	TYPE(PERIPHC_I2S4,	CLOCK_TYPE_AXPT),
	TYPE(PERIPHC_I2C4,	CLOCK_TYPE_PC2CC3M_T16),
	TYPE(PERIPHC_SBC5,	CLOCK_TYPE_PC2CC3M_T),
	TYPE(PERIPHC_SBC6,	CLOCK_TYPE_PC2CC3M_T),

	/* 0x48 */
	TYPE(PERIPHC_AUDIO,	CLOCK_TYPE_AC2CC3P_TS2),
	TYPE(PERIPHC_49h,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_DAM0,	CLOCK_TYPE_AC2CC3P_TS2),
	TYPE(PERIPHC_DAM1,	CLOCK_TYPE_AC2CC3P_TS2),
	TYPE(PERIPHC_DAM2,	CLOCK_TYPE_AC2CC3P_TS2),
	TYPE(PERIPHC_HDA2CODEC2X, CLOCK_TYPE_PC2CC3M_T),
	TYPE(PERIPHC_ACTMON,	CLOCK_TYPE_PC2CC3S_T),
	TYPE(PERIPHC_EXTPERIPH1, CLOCK_TYPE_ASPTE),

	/* 0x50 */
	TYPE(PERIPHC_EXTPERIPH2, CLOCK_TYPE_ASPTE),
	TYPE(PERIPHC_EXTPERIPH3, CLOCK_TYPE_ASPTE),
	TYPE(PERIPHC_52h,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_I2CSLOW,	CLOCK_TYPE_PC2CC3S_T),
	TYPE(PERIPHC_SYS,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_55h,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_56h,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_57h,	CLOCK_TYPE_NONE),

	/* 0x58 */
	TYPE(PERIPHC_58h,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_59h,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_5ah,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_5bh,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_SATAOOB,	CLOCK_TYPE_PCMT),
	TYPE(PERIPHC_SATA,	CLOCK_TYPE_PCMT),
	TYPE(PERIPHC_HDA,	CLOCK_TYPE_PC2CC3M_T),
	TYPE(PERIPHC_5fh,	CLOCK_TYPE_NONE),

	/* 0x60 */
	TYPE(PERIPHC_XUSB_CORE_HOST, CLOCK_TYPE_NONE),
	TYPE(PERIPHC_XUSB_FALCON, CLOCK_TYPE_NONE),
	TYPE(PERIPHC_XUSB_FS,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_XUSB_CORE_DEV, CLOCK_TYPE_NONE),
	TYPE(PERIPHC_XUSB_SS,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_CILAB,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_CILCD,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_CILE,	CLOCK_TYPE_NONE),

	/* 0x68 */
	TYPE(PERIPHC_DSIA_LP,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_DSIB_LP,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_ENTROPY,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_DVFS_REF,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_DVFS_SOC,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_TRACECLKIN, CLOCK_TYPE_NONE),
	TYPE(PERIPHC_ADX0,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_AMX0,	CLOCK_TYPE_NONE),

	/* 0x70 */
	TYPE(PERIPHC_EMC_LATENCY, CLOCK_TYPE_NONE),
	TYPE(PERIPHC_SOC_THERM,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_72h,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_73h,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_74h,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_75h,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_VI_SENSOR2, CLOCK_TYPE_NONE),
	TYPE(PERIPHC_I2C6,	CLOCK_TYPE_PC2CC3M_T16),

	/* 0x78 */
	TYPE(PERIPHC_78h,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_EMC_DLL,	CLOCK_TYPE_MCPTM2C2C3),
	TYPE(PERIPHC_HDMI_AUDIO, CLOCK_TYPE_NONE),
	TYPE(PERIPHC_CLK72MHZ,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_ADX1,	CLOCK_TYPE_AC2CC3P_TS2),
	TYPE(PERIPHC_AMX1,	CLOCK_TYPE_AC2CC3P_TS2),
	TYPE(PERIPHC_VIC,	CLOCK_TYPE_NONE),
	TYPE(PERIPHC_7Fh,	CLOCK_TYPE_NONE),
};

/*
 * This array translates a periph_id to a periphc_internal_id
 *
 * Not present/matched up:
 *	uint vi_sensor;	 _VI_SENSOR_0,		0x1A8
 *	SPDIF - which is both 0x08 and 0x0c
 *
 */
#define NONE(name) (-1)
#define OFFSET(name, value) PERIPHC_ ## name
static s8 periph_id_to_internal_id[PERIPH_ID_COUNT] = {
	/* Low word: 31:0 */
	NONE(CPU),
	NONE(COP),
	NONE(TRIGSYS),
	NONE(ISPB),
	NONE(RESERVED4),
	NONE(TMR),
	PERIPHC_UART1,
	PERIPHC_UART2,	/* and vfir 0x68 */

	/* 8 */
	NONE(GPIO),
	PERIPHC_SDMMC2,
	PERIPHC_SPDIF_IN,
	PERIPHC_I2S1,
	PERIPHC_I2C1,
	NONE(RESERVED13),
	PERIPHC_SDMMC1,
	PERIPHC_SDMMC4,

	/* 16 */
	NONE(TCW),
	PERIPHC_PWM,
	PERIPHC_I2S2,
	NONE(RESERVED19),
	PERIPHC_VI,
	NONE(RESERVED21),
	NONE(USBD),
	NONE(ISP),

	/* 24 */
	NONE(RESERVED24),
	NONE(RESERVED25),
	PERIPHC_DISP2,
	PERIPHC_DISP1,
	PERIPHC_HOST1X,
	NONE(VCP),
	PERIPHC_I2S0,
	NONE(CACHE2),

	/* Middle word: 63:32 */
	NONE(MEM),
	NONE(AHBDMA),
	NONE(APBDMA),
	NONE(RESERVED35),
	NONE(RESERVED36),
	NONE(STAT_MON),
	NONE(RESERVED38),
	NONE(FUSE),

	/* 40 */
	NONE(KFUSE),
	PERIPHC_SBC1,		/* SBCx = SPIx */
	PERIPHC_NOR,
	NONE(RESERVED43),
	PERIPHC_SBC2,
	NONE(XIO),
	PERIPHC_SBC3,
	PERIPHC_I2C5,

	/* 48 */
	NONE(DSI),
	NONE(RESERVED49),
	PERIPHC_HSI,
	PERIPHC_HDMI,
	NONE(CSI),
	NONE(RESERVED53),
	PERIPHC_I2C2,
	PERIPHC_UART3,

	/* 56 */
	NONE(MIPI_CAL),
	PERIPHC_EMC,
	NONE(USB2),
	NONE(USB3),
	NONE(RESERVED60),
	PERIPHC_VDE,
	NONE(BSEA),
	NONE(BSEV),

	/* Upper word 95:64 */
	NONE(RESERVED64),
	PERIPHC_UART4,
	PERIPHC_UART5,
	PERIPHC_I2C3,
	PERIPHC_SBC4,
	PERIPHC_SDMMC3,
	NONE(PCIE),
	PERIPHC_OWR,

	/* 72 */
	NONE(AFI),
	PERIPHC_CSITE,
	NONE(PCIEXCLK),
	NONE(AVPUCQ),
	NONE(LA),
	NONE(TRACECLKIN),
	NONE(SOC_THERM),
	NONE(DTV),

	/* 80 */
	NONE(RESERVED80),
	PERIPHC_I2CSLOW,
	NONE(DSIB),
	PERIPHC_TSEC,
	NONE(RESERVED84),
	NONE(RESERVED85),
	NONE(RESERVED86),
	NONE(EMUCIF),

	/* 88 */
	NONE(RESERVED88),
	NONE(XUSB_HOST),
	NONE(RESERVED90),
	PERIPHC_MSENC,
	NONE(RESERVED92),
	NONE(RESERVED93),
	NONE(RESERVED94),
	NONE(XUSB_DEV),

	/* V word: 31:0 */
	NONE(CPUG),
	NONE(CPULP),
	NONE(V_RESERVED2),
	PERIPHC_MSELECT,
	NONE(V_RESERVED4),
	PERIPHC_I2S3,
	PERIPHC_I2S4,
	PERIPHC_I2C4,

	/* 104 */
	PERIPHC_SBC5,
	PERIPHC_SBC6,
	PERIPHC_AUDIO,
	NONE(APBIF),
	PERIPHC_DAM0,
	PERIPHC_DAM1,
	PERIPHC_DAM2,
	PERIPHC_HDA2CODEC2X,

	/* 112 */
	NONE(ATOMICS),
	NONE(V_RESERVED17),
	NONE(V_RESERVED18),
	NONE(V_RESERVED19),
	NONE(V_RESERVED20),
	NONE(V_RESERVED21),
	NONE(V_RESERVED22),
	PERIPHC_ACTMON,

	/* 120 */
	NONE(EXTPERIPH1),
	NONE(EXTPERIPH2),
	NONE(EXTPERIPH3),
	NONE(OOB),
	PERIPHC_SATA,
	PERIPHC_HDA,
	NONE(TZRAM),
	NONE(SE),

	/* W word: 31:0 */
	NONE(HDA2HDMICODEC),
	NONE(SATACOLD),
	NONE(W_RESERVED2),
	NONE(W_RESERVED3),
	NONE(W_RESERVED4),
	NONE(W_RESERVED5),
	NONE(W_RESERVED6),
	NONE(W_RESERVED7),

	/* 136 */
	NONE(CEC),
	NONE(W_RESERVED9),
	NONE(W_RESERVED10),
	NONE(W_RESERVED11),
	NONE(W_RESERVED12),
	NONE(W_RESERVED13),
	NONE(XUSB_PADCTL),
	NONE(W_RESERVED15),

	/* 144 */
	NONE(W_RESERVED16),
	NONE(W_RESERVED17),
	NONE(W_RESERVED18),
	NONE(W_RESERVED19),
	NONE(W_RESERVED20),
	NONE(ENTROPY),
	NONE(DDS),
	NONE(W_RESERVED23),

	/* 152 */
	NONE(DP2),
	NONE(AMX0),
	NONE(ADX0),
	NONE(DVFS),
	NONE(XUSB_SS),
	NONE(W_RESERVED29),
	NONE(W_RESERVED30),
	NONE(W_RESERVED31),

	/* X word: 31:0 */
	NONE(SPARE),
	NONE(X_RESERVED1),
	NONE(X_RESERVED2),
	NONE(X_RESERVED3),
	NONE(CAM_MCLK),
	NONE(CAM_MCLK2),
	PERIPHC_I2C6,
	NONE(X_RESERVED7),

	/* 168 */
	NONE(X_RESERVED8),
	NONE(X_RESERVED9),
	NONE(X_RESERVED10),
	NONE(VIM2_CLK),
	NONE(X_RESERVED12),
	NONE(X_RESERVED13),
	NONE(EMC_DLL),
	NONE(X_RESERVED15),

	/* 176 */
	NONE(HDMI_AUDIO),
	NONE(CLK72MHZ),
	NONE(VIC),
	NONE(X_RESERVED19),
	NONE(ADX1),
	NONE(DPAUX),
	NONE(SOR0),
	NONE(X_RESERVED23),

	/* 184 */
	NONE(GPU),
	NONE(AMX1),
	NONE(X_RESERVED26),
	NONE(X_RESERVED27),
	NONE(X_RESERVED28),
	NONE(X_RESERVED29),
	NONE(X_RESERVED30),
	NONE(X_RESERVED31),
};

/*
 * Get the oscillator frequency, from the corresponding hardware configuration
 * field. Note that Tegra30+ support 3 new higher freqs, but we map back
 * to the old T20 freqs. Support for the higher oscillators is TBD.
 */
enum clock_osc_freq clock_get_osc_freq(void)
{
	struct clk_rst_ctlr *clkrst =
			(struct clk_rst_ctlr *)NV_PA_CLK_RST_BASE;
	u32 reg;

	reg = readl(&clkrst->crc_osc_ctrl);
	reg = (reg & OSC_FREQ_MASK) >> OSC_FREQ_SHIFT;

	if (reg & 1)				/* one of the newer freqs */
		printf("Warning: OSC_FREQ is unsupported! (%d)\n", reg);

	return reg >> 2;	/* Map to most common (T20) freqs */
}

/* Returns a pointer to the clock source register for a peripheral */
u32 *get_periph_source_reg(enum periph_id periph_id)
{
	struct clk_rst_ctlr *clkrst =
		(struct clk_rst_ctlr *)NV_PA_CLK_RST_BASE;
	enum periphc_internal_id internal_id;

	/* Coresight is a special case */
	if (periph_id == PERIPH_ID_CSI)
		return &clkrst->crc_clk_src[PERIPH_ID_CSI+1];

	assert(periph_id >= PERIPH_ID_FIRST && periph_id < PERIPH_ID_COUNT);
	internal_id = periph_id_to_internal_id[periph_id];
	assert(internal_id != -1);
	if (internal_id >= PERIPHC_VW_FIRST) {
		internal_id -= PERIPHC_VW_FIRST;
		return &clkrst->crc_clk_src_vw[internal_id];
	} else {
		return &clkrst->crc_clk_src[internal_id];
	}
}

/**
 * Given a peripheral ID and the required source clock, this returns which
 * value should be programmed into the source mux for that peripheral.
 *
 * There is special code here to handle the one source type with 5 sources.
 *
 * @param periph_id	peripheral to start
 * @param source	PLL id of required parent clock
 * @param mux_bits	Set to number of bits in mux register: 2 or 4
 * @param divider_bits Set to number of divider bits (8 or 16)
 * @return mux value (0-4, or -1 if not found)
 */
int get_periph_clock_source(enum periph_id periph_id,
	enum clock_id parent, int *mux_bits, int *divider_bits)
{
	enum clock_type_id type;
	enum periphc_internal_id internal_id;
	int mux;

	assert(clock_periph_id_isvalid(periph_id));

	internal_id = periph_id_to_internal_id[periph_id];
	assert(periphc_internal_id_isvalid(internal_id));

	type = clock_periph_type[internal_id];
	assert(clock_type_id_isvalid(type));

	*mux_bits = clock_source[type][CLOCK_MAX_MUX];

	if (type == CLOCK_TYPE_PC2CC3M_T16)
		*divider_bits = 16;
	else
		*divider_bits = 8;

	for (mux = 0; mux < CLOCK_MAX_MUX; mux++)
		if (clock_source[type][mux] == parent)
			return mux;

	/* if we get here, either us or the caller has made a mistake */
	printf("Caller requested bad clock: periph=%d, parent=%d\n", periph_id,
	       parent);
	return -1;
}

void clock_set_enable(enum periph_id periph_id, int enable)
{
	struct clk_rst_ctlr *clkrst =
		(struct clk_rst_ctlr *)NV_PA_CLK_RST_BASE;
	u32 *clk;
	u32 reg;

	/* Enable/disable the clock to this peripheral */
	assert(clock_periph_id_isvalid(periph_id));
	if ((int)periph_id < (int)PERIPH_ID_VW_FIRST)
		clk = &clkrst->crc_clk_out_enb[PERIPH_REG(periph_id)];
	else
		clk = &clkrst->crc_clk_out_enb_vw[PERIPH_REG(periph_id)];
	reg = readl(clk);
	if (enable)
		reg |= PERIPH_MASK(periph_id);
	else
		reg &= ~PERIPH_MASK(periph_id);
	writel(reg, clk);
}

void reset_set_enable(enum periph_id periph_id, int enable)
{
	struct clk_rst_ctlr *clkrst =
		(struct clk_rst_ctlr *)NV_PA_CLK_RST_BASE;
	u32 *reset;
	u32 reg;

	/* Enable/disable reset to the peripheral */
	assert(clock_periph_id_isvalid(periph_id));
	if (periph_id < PERIPH_ID_VW_FIRST)
		reset = &clkrst->crc_rst_dev[PERIPH_REG(periph_id)];
	else
		reset = &clkrst->crc_rst_dev_vw[PERIPH_REG(periph_id)];
	reg = readl(reset);
	if (enable)
		reg |= PERIPH_MASK(periph_id);
	else
		reg &= ~PERIPH_MASK(periph_id);
	writel(reg, reset);
}

#ifdef CONFIG_OF_CONTROL
/*
 * Convert a device tree clock ID to our peripheral ID. They are mostly
 * the same but we are very cautious so we check that a valid clock ID is
 * provided.
 *
 * @param clk_id    Clock ID according to tegra124 device tree binding
 * @return peripheral ID, or PERIPH_ID_NONE if the clock ID is invalid
 */
enum periph_id clk_id_to_periph_id(int clk_id)
{
	if (clk_id > PERIPH_ID_COUNT)
		return PERIPH_ID_NONE;

	switch (clk_id) {
	case PERIPH_ID_RESERVED4:
	case PERIPH_ID_RESERVED25:
	case PERIPH_ID_RESERVED35:
	case PERIPH_ID_RESERVED36:
	case PERIPH_ID_RESERVED38:
	case PERIPH_ID_RESERVED43:
	case PERIPH_ID_RESERVED49:
	case PERIPH_ID_RESERVED53:
	case PERIPH_ID_RESERVED64:
	case PERIPH_ID_RESERVED84:
	case PERIPH_ID_RESERVED85:
	case PERIPH_ID_RESERVED86:
	case PERIPH_ID_RESERVED88:
	case PERIPH_ID_RESERVED90:
	case PERIPH_ID_RESERVED92:
	case PERIPH_ID_RESERVED93:
	case PERIPH_ID_RESERVED94:
	case PERIPH_ID_V_RESERVED2:
	case PERIPH_ID_V_RESERVED4:
	case PERIPH_ID_V_RESERVED17:
	case PERIPH_ID_V_RESERVED18:
	case PERIPH_ID_V_RESERVED19:
	case PERIPH_ID_V_RESERVED20:
	case PERIPH_ID_V_RESERVED21:
	case PERIPH_ID_V_RESERVED22:
	case PERIPH_ID_W_RESERVED2:
	case PERIPH_ID_W_RESERVED3:
	case PERIPH_ID_W_RESERVED4:
	case PERIPH_ID_W_RESERVED5:
	case PERIPH_ID_W_RESERVED6:
	case PERIPH_ID_W_RESERVED7:
	case PERIPH_ID_W_RESERVED9:
	case PERIPH_ID_W_RESERVED10:
	case PERIPH_ID_W_RESERVED11:
	case PERIPH_ID_W_RESERVED12:
	case PERIPH_ID_W_RESERVED13:
	case PERIPH_ID_W_RESERVED15:
	case PERIPH_ID_W_RESERVED16:
	case PERIPH_ID_W_RESERVED17:
	case PERIPH_ID_W_RESERVED18:
	case PERIPH_ID_W_RESERVED19:
	case PERIPH_ID_W_RESERVED20:
	case PERIPH_ID_W_RESERVED23:
	case PERIPH_ID_W_RESERVED29:
	case PERIPH_ID_W_RESERVED30:
	case PERIPH_ID_W_RESERVED31:
		return PERIPH_ID_NONE;
	default:
		return clk_id;
	}
}
#endif /* CONFIG_OF_CONTROL */

void clock_early_init(void)
{
	struct clk_rst_ctlr *clkrst =
		(struct clk_rst_ctlr *)NV_PA_CLK_RST_BASE;

	tegra30_set_up_pllp();

	/*
	 * PLLC output frequency set to 600Mhz
	 * PLLD output frequency set to 925Mhz
	 */
	switch (clock_get_osc_freq()) {
	case CLOCK_OSC_FREQ_12_0: /* OSC is 12Mhz */
		clock_set_rate(CLOCK_ID_CGENERAL, 600, 12, 0, 8);
		clock_set_rate(CLOCK_ID_DISPLAY, 925, 12, 0, 12);
		break;

	case CLOCK_OSC_FREQ_26_0: /* OSC is 26Mhz */
		clock_set_rate(CLOCK_ID_CGENERAL, 600, 26, 0, 8);
		clock_set_rate(CLOCK_ID_DISPLAY, 925, 26, 0, 12);
		break;

	case CLOCK_OSC_FREQ_13_0: /* OSC is 13Mhz */
		clock_set_rate(CLOCK_ID_CGENERAL, 600, 13, 0, 8);
		clock_set_rate(CLOCK_ID_DISPLAY, 925, 13, 0, 12);
		break;
	case CLOCK_OSC_FREQ_19_2:
	default:
		/*
		 * These are not supported. It is too early to print a
		 * message and the UART likely won't work anyway due to the
		 * oscillator being wrong.
		 */
		break;
	}

	/* PLLC_MISC2: Set dynramp_stepA/B. MISC2 maps to pll_out[1] */
	writel(0x00561600, &clkrst->crc_pll[CLOCK_ID_CGENERAL].pll_out[1]);

	/* PLLC_MISC: Set LOCK_ENABLE */
	writel(0x01000000, &clkrst->crc_pll[CLOCK_ID_CGENERAL].pll_misc);
	udelay(2);

	/* PLLD_MISC: Set CLKENABLE, CPCON 12, LFCON 1 */
	writel(0x40000C10, &clkrst->crc_pll[CLOCK_ID_DISPLAY].pll_misc);
	udelay(2);
}

void arch_timer_init(void)
{
	struct sysctr_ctlr *sysctr = (struct sysctr_ctlr *)NV_PA_TSC_BASE;
	u32 freq, val;

	freq = clock_get_rate(CLOCK_ID_OSC);
	debug("%s: osc freq is %dHz [0x%08X]\n", __func__, freq, freq);

	/* ARM CNTFRQ */
	asm("mcr p15, 0, %0, c14, c0, 0\n" : : "r" (freq));

	/* Only Tegra114+ has the System Counter regs */
	debug("%s: setting CNTFID0 to 0x%08X\n", __func__, freq);
	writel(freq, &sysctr->cntfid0);

	val = readl(&sysctr->cntcr);
	val |= TSC_CNTCR_ENABLE | TSC_CNTCR_HDBG;
	writel(val, &sysctr->cntcr);
	debug("%s: TSC CNTCR = 0x%08X\n", __func__, val);
}
