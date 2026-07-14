/*
 * max96724.c - MAX96724 GMSL2 Quad Deserializer driver (Tunnel Mode)
 *
 * Copyright (c) 2026, RealSense, Inc.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include <linux/gpio.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <media/camera_common.h>
#include <linux/module.h>
#include <media/max96724.h>

/* ================================================================
 * Register Addresses
 * ================================================================ */

/* --- Device ID / Configuration --- */

/* REG0: Device ID (read-only, returns 0xA2) */
#define MAX96724_DEV_ID_ADDR		0x00

/* ERRB master reset register */
#define MAX96724_ERRB_MST_RST_ADDR	0x05

/*
 *   GMSL Link/PHY Enable and Mode Select
 *   bits[7:4]: PHY mode (0=GMSL1, 1=GMSL2) per link D/C/B/A
 *   bits[3:0]: Link Enable per link D/C/B/A
 */
#define MAX96724_LINK_EN_ADDR		0x0006

/*
 *   GMSL Link/PHY Rate Select (Links A&B)
 *   bits[7:6]: Tx Rate Link B, bits[5:4]: Rx Rate Link B
 *   bits[3:2]: Tx Rate Link A, bits[1:0]: Rx Rate Link A
 *   Tx: 00=187.5Mbps, Rx: 01=3Gbps, 10=6Gbps
 */
#define MAX96724_LINK_RATE_AB_ADDR	0x0010

/* GMSL Link/PHY Rate Select (Links C&D) */
#define MAX96724_LINK_RATE_CD_ADDR	0x0011

/* Link lock status (bit[3]=LOCK). */
#define MAX96724_LINK_A_LOCK_ADDR	0x001A
#define MAX96724_LINK_B_LOCK_ADDR	0x001B
#define MAX96724_LINK_C_LOCK_ADDR	0x001C
#define MAX96724_LINK_D_LOCK_ADDR	0x001D

/*
 *   GMSL Link Reset
 *   bits[7:4]: Link reset D/C/B/A, bits[3:0]: One-shot reset D/C/B/A
 */
#define MAX96724_ONESHOT_ADDR		0x0018

/* Soft Reset (write 0x40, wait 5ms) */
#define MAX96724_REG13_ADDR		0x000D

/*
 *   Error Channel Power Up registers (required for 6Gbps)
 *   Write 0x75 immediately after power-up for robust 6Gbps operation
 */
#define MAX96724_ERRCH_A_ADDR		0x1449
#define MAX96724_ERRCH_B_ADDR		0x1549
#define MAX96724_ERRCH_C_ADDR		0x1649
#define MAX96724_ERRCH_D_ADDR		0x1749
#define MAX96724_ERRCH_FORCE_ON		0x75

/* --- I2C Control Channel / Errata --- */

/*
 *   Tunnel Mode SRAM LCRC error suppression
 *   Set 0x458[7:4] = 0x0 to suppress spurious ERRB in tunnel mode
 */
#define MAX96724_SRAM_LCRC_ERR_ADDR	0x0458

/* --- Video Pipe Selection --- */

/*
 *   bits[3:0] = Pipe 0 source (0x0=SIOA)
 *   bits[7:4] = Pipe 1 source (0x1=SIOB)
 */
#define MAX96724_PIPE_SEL0_ADDR		0xF0

/*
 *   bits[3:0] = Pipe 2 source (0x2=SIOC)
 *   bits[7:4] = Pipe 3 source (0x3=SIOD)
 */
#define MAX96724_PIPE_SEL1_ADDR		0xF1

#define MAX96724_PIPE_EN_ADDR		0xF4

/* --- Pipe Receiver / Tunnel Mode --- */

/*
 *   bit[5]=1: Tunnel Mode, bits[1:0]=11: 4-lane MIPI out
 *   Stride: 0x12 per pipe (P0=0x100, P1=0x112, P2=0x124, P3=0x136)
 */
#define MAX96724_VID_RX0_P0_ADDR	0x100
#define MAX96724_VID_RX6_P0_ADDR	0x106
#define MAX96724_VID_RX_STRIDE		0x12

#define MAX96724_VID_RX1_P0_ADDR	0x101

/* --- CSI-2 Output (BACKTOP) --- */

/* BACKTOP: CSI-2 output port enable
 *   0x040B bit[1]=CSIB_EN, bit[0]=CSIA_EN
 */
#define MAX96724_BACKTOP_EN_ADDR	0x040B

/* --- MIPI DPLL (Data Rate) --- */

/*
 *   MIPI PHY DPLL Freq registers
 *   bits[4:0] = frequency in multiples of 100MHz
 *   bit[5]    = DPLL enable (must be set for DPLL to lock)
 *   E.g., 0x27 = 700MHz + enable = 700Mbps/lane D-PHY
 *   E.g., 0x2F = 1500MHz + enable = 1.5Gbps/lane
 */
#define MAX96724_DPLL_FREQ0_ADDR	0x0415
#define MAX96724_DPLL_FREQ1_ADDR	0x0418
#define MAX96724_DPLL_FREQ2_ADDR	0x041B
#define MAX96724_DPLL_FREQ3_ADDR	0x041E

/*
 *   DPLL PHY reset control
 *   0x1D00 = DPLL CSI PHY1 control
 *   Write 0xF4 to disable/reset, 0xF5 to enable
 */
#define MAX96724_DPLL_CSI2_ADDR		0x1D00
#define MAX96724_DPLL_DISABLE		0xF4
#define MAX96724_DPLL_ENABLE		0xF5

/* --- CSI-2 Output PHY Configuration --- */

/*
 * CSI output mode/config register.
 * Mode bits (set exactly one of [4:0]):
 *   bit[0]=0x01: 4x2 mode
 *   bit[2]=0x04: 2x4 mode        (Ctrl 1 = master for Port A, Ctrl 2 = Port B)
 *   bit[3]=0x08: 1x4a + 2x2 mode (Ctrl 1 = master for Port A)
 *   bit[4]=0x10: 1x4b + 2x2 mode (Ctrl 2 = master for Port B)
 *
 * Clock-force bits:
 *   bit[7]=0x80: force ALL MIPI clocks running (continuous HS clock)
 *   bit[6]=0x40: force PHY 3 MIPI clock running
 *   bit[5]=0x20: force PHY 0 MIPI clock running
 */
#define MAX96724_CSI_OUT_CFG_ADDR	0x08A0

/*
 *   MIPI PHY Enable register
 *   bits[7:4]: Enable PHY 3/2/1/0 (1=enabled, 0=standby)
 */
#define MAX96724_CSI_PHY_EN_ADDR	0x08A2

/* MIPI PHY 0 and 1 Lane Mapping */
#define MAX96724_CSI_LANE_MAP1_ADDR	0x08A3

/* MIPI PHY 2 and 3 Lane Mapping */
#define MAX96724_CSI_LANE_MAP2_ADDR	0x08A4

/*
 *   MIPI output lane polarity (P/N swap) registers.
 *   0x08A5 = PHY0/PHY1 lane polarity, 0x08A6 = PHY2/PHY3 lane polarity.
 *   The FangZhu FG24-4CH PCB swaps the differential P/N pairs on the
 *   MAX96724 CSI output, so the deserializer must invert lane polarity
 */
#define MAX96724_CSI_PHY_POL0_ADDR	0x08A5
#define MAX96724_CSI_PHY_POL1_ADDR	0x08A6
#define MAX96724_CSI_PHY_POL_SWAP	0x3F

/* Pipe-to-controller mapping */
#define MAX96724_MIPI_CTRL_SEL_ADDR	0x08CA

/* Method-1 controller mapping */
#define MAX96724_MIPI_CTRL_SEL_PIXEL	0xE4

/* --- Lane Control per pipe (stride 0x40) --- */

/*
 *   0xC0 = 4-lane enabled
 */
#define MAX96724_LANE_CTRL0_ADDR	0x090A
#define MAX96724_LANE_CTRL1_ADDR	0x094A
#define MAX96724_LANE_CTRL2_ADDR	0x098A
#define MAX96724_LANE_CTRL3_ADDR	0x09CA

/* --- Pipe Mapping Registers (stride 0x40 per pipe) --- */

/*
 *   Base addresses for Pipe X (Pipe 0)
 *   Stride: 0x40 per pipe
 */
#define MAX96724_TX11_PIPE_X_EN_ADDR		0x090B
#define MAX96724_TX45_PIPE_X_DST_CTRL_ADDR	0x092D
#define MAX96724_PIPE_X_SRC_0_MAP_ADDR		0x090D
#define MAX96724_PIPE_X_DST_0_MAP_ADDR		0x090E
#define MAX96724_PIPE_X_SRC_1_MAP_ADDR		0x090F
#define MAX96724_PIPE_X_DST_1_MAP_ADDR		0x0910
#define MAX96724_PIPE_X_SRC_2_MAP_ADDR		0x0911
#define MAX96724_PIPE_X_DST_2_MAP_ADDR		0x0912
#define MAX96724_PIPE_X_SRC_3_MAP_ADDR		0x0913
#define MAX96724_PIPE_X_DST_3_MAP_ADDR		0x0914

/* --- Stream Select --- */

/*
 *   Pipe X=0x0933, stride 0x40 per pipe
 */
#define MAX96724_PIPE_X_ST_SEL_ADDR	0x0933

/* --- Extended Pipe Mapping (DST_CTRL2-4 / TX49) --- */

/*
 *   Extended DST_CTRL for additional mapping pairs
 *   All to PHY1 (0x55), same as TX45
 *   Stride: 0x40 per pipe (base = Pipe X)
 */
#define MAX96724_TX46_PIPE_X_ADDR	0x092E
#define MAX96724_TX47_PIPE_X_ADDR	0x092F
#define MAX96724_TX48_PIPE_X_ADDR	0x0930

/*
 *   MIPI_TX49 synchronous concatenation control
 *   Set to 0x00 (no concat) for single-camera tunnel mode.
 *   For 4W concat use 0x87.
 *   Stride: 0x40 per pipe (base = Pipe X)
 */
#define MAX96724_TX49_PIPE_X_ADDR	0x0931
#define MAX96724_TX49_CONCAT_4W		0x87

/* --- Tunnel Mode per-pipe --- */

/*
 *   bit[0] = TUN_EN, Default=0x08, write 0x01 to enable
 */
#define MAX96724_PIPE0_TUN_EN_ADDR	0x0936
#define MAX96724_PIPE1_TUN_EN_ADDR	0x0976
#define MAX96724_PIPE2_TUN_EN_ADDR	0x09B6
#define MAX96724_PIPE3_TUN_EN_ADDR	0x09F6

/*
 *   bit[6]: disable auto-tunnel-detect
 *   bits[5:4]: TUN_DEST (00=PHY0, 01=PHY1, 10=PHY2, 11=PHY3)
 */
#define MAX96724_PIPE0_TUN_DEST_ADDR	0x0939
#define MAX96724_PIPE1_TUN_DEST_ADDR	0x0979
#define MAX96724_PIPE2_TUN_DEST_ADDR	0x09B9
#define MAX96724_PIPE3_TUN_DEST_ADDR	0x09F9

/* ================================================================
 * Register Values
 * ================================================================ */

/* Soft reset value (write to 0x000D) */
#define MAX96724_SOFT_RESET		0x40

/*
 * Link Rate values for 0x0010/0x0011
 *   Per link pair: bits[1:0]=RxA, [3:2]=TxA, [5:4]=RxB, [7:6]=TxB
 *   Tx = 00 (187.5Mbps), Rx = 01 (3Gbps) or 10 (6Gbps)
 *   3Gbps: Tx=00, Rx=01 → per pair 0x01, both pairs = 0x01 in low nibble
 */
#define MAX96724_LINK_RATE_3GBPS	0x01  /* Rx=3G, Tx=187.5M for one link pair */
#define MAX96724_LINK_RATE_6GBPS	0x02  /* Rx=6G, Tx=187.5M for one link pair */

/*
 * 0x0006: Link enable / mode values
 *   bits[7:4] = PHY mode (1=GMSL2 per link)
 *   bits[3:0] = Link enable per link
 *   0xF1 = all GMSL2-mode, only Link A enabled
 *   0xFF = all GMSL2-mode, all 4 links enabled
 */
#define MAX96724_LINK_EN_SIOA		0xF1  /* GMSL2 all, enable Link A only */
#define MAX96724_LINK_EN_ALL		0xFF  /* GMSL2 all, enable all links */

#define MAX96724_LINK_LOCKED		0x08
#define MAX96724_LINK_LOCK_POLL_MS	100
#define MAX96724_LINK_LOCK_TIMEOUT_MS	5000

/* BACKTOP_EN (0x040B): CSI output port enable */
#define MAX96724_BACKTOP_CSIB_EN	0x02
#define MAX96724_BACKTOP_CSIA_EN	0x01

/*
 *   VIDEO_RX0: DIS_PKT_DET=1 (bit0) required for tunnel mode.
 *   Without it, DES rejects tunnel data as invalid GMSL2 video packets.
 *   SEQ_MISS_EN=1 (bit4), LINE_CRC_EN=1 (bit1).  XML value: 0x33.
 */
#define MAX96724_VID_RX0_CFG_VAL	0x33
#define MAX96724_VID_RX6_CFG_VAL	0x0A

/* Pixel-mode receiver settings */
#define MAX96724_VID_RX0_PIXEL_VAL	0x32
#define MAX96724_VID_RX6_PIXEL_VAL	0x12

/*
 *   Tunnel mode enable (MIPI_TX54, 0x0936):
 *   bit[0] = TUN_EN = 1
 *   bits[6:5] = DESKEW_TUN = 1, periodic deskew follows SER.
 *   This keeps the MAX96724 D-PHY output deskew cadence aligned with
 *   the tunneled CSI-2 source when HKR->SER runs at >= 1.5Gbps/lane.
 */
#define MAX96724_TUN_EN			0x21

/* Default/reset state when tunnel is disabled */
#define MAX96724_TUN_DISABLED		0x08

/*
 *   Tunnel controller destination (MIPI_TX57, 0x0939):
 *   bits[5:4]: TUN_DEST (00=PHY0, 01=PHY1, 10=PHY2, 11=PHY3)
 *   Verified value: 0x10 = PHY1 destination with auto-detect enabled
 */
#define MAX96724_TUN_DEST_CTRL0		0x00  /* Auto-detect + PHY0 */
#define MAX96724_TUN_DEST_CTRL1		0x10  /* Auto-detect + PHY1 (Port A master) */

#define MAX96724_CSI_MODE_2X4_VAL	0x04  /* bit[2] = clean 2x4: Port A = PHY0+PHY1 */

/*
 *   0x84 = 2x4 mode (bit2) + bit[7] "force ALL MIPI clocks running".  The bit[7]
 *   clock-force is the key part: it keeps the D-PHY clock lane in continuous HS
 *   mode so the Orin NVCSI/CIL can lock.
 */
#define MAX96724_CSI_OUT_EN_VAL		0x84  /* 2x4 + bit[7] force all MIPI clocks running */

#define MAX96724_CSI_PHY_EN_ALL		0xF0
#define MAX96724_CSI_PHY_EN_01		0x30  /* PHY0+PHY1 only (Port A) */
#define MAX96724_CSI_PHY_EN_23		0xC0  /* PHY2+PHY3 only (Port B) */

/* Lane mapping default */
#define MAX96724_CSI_LANE_MAP_DEFAULT	0xE4

/* Lane control: 4-lane enabled */
#define MAX96724_LANE_CTRL_4LANE	0xC0

/*
 *   Lane control: 2-lane enabled.
 *   bits[7:6] = (num_lanes - 1): 01=2-lane(0x40), 11=4-lane(0xC0)
 *   Ref: gmsl_ser_des_guide §8.2, E2E_enable_guide §5.6.4
 */
#define MAX96724_LANE_CTRL_2LANE	0x40

/*
 *   CSI PHY enable: all PHYs powered for DPLL lock.
 */
#define MAX96724_CSI_PHY_EN_2LANE	0xF0

/*
 *   DPLL for Controller 0 (drives PHY0, 2 lanes).
 *   bits[4:0] = data-rate / 100 MHz.  700 Mbps / 100 = 7 = 0x07.
 *   bit[5]    = DPLL enable = 1.
 *   Result: 0x27 = 700 Mbps/lane D-PHY.
 */
#define MAX96724_DPLL_700MBPS_CTRL0	0x27

/*
 *   CSI-output DPLL data rate = 2000 Mbps.
 *   bits[4:0] = data-rate / 100 MHz -> 2000/100 = 20 = 0x14. bit[5] = DPLL en.
 */
#define MAX96724_DPLL_2000MBPS		0x34

/* One-shot reset: all links */
#define MAX96724_ONESHOT_ALL		0x0F

/*
 *   Pipe source selection values
 *   Encoding per pipe nibble in tunnel mode:
 *   0x22 routes all pipes from Link A in tunnel mode.
 */
#define MAX96724_PIPE_SEL0_SINGLE	0x22  /* All pipes from Link A */
#define MAX96724_PIPE_SEL1_SINGLE	0x22  /* All pipes from Link A */
#define MAX96724_PIPE_SEL0_PIXEL	0x62  /* pixel-mode reference */
#define MAX96724_PIPE_SEL1_PIXEL	0xEA  /* pixel-mode reference */
#define MAX96724_PIPE_SEL0_DUAL		0x40  /* Pipe 0 ← A/X, Pipe 1 ← B/X */
#define MAX96724_PIPE_SEL0_QUAD_LO	0x40  /* Pipe 0 ← A/X, Pipe 1 ← B/X */
#define MAX96724_PIPE_SEL1_QUAD_HI	0xC8  /* Pipe 2 ← C/X, Pipe 3 ← D/X */

/* Pipe enable values */
#define MAX96724_PIPE_EN_1		0x01
#define MAX96724_PIPE_EN_2		0x03
#define MAX96724_PIPE_EN_4		0x0F

/* Lane control map: bits[7:6] = (num_lanes - 1) */
#define MAX96724_LANE_CTRL_MAP(num_lanes) \
	((((num_lanes) - 1) << 6) & 0xC0)

/* Reset all */
#define MAX96724_RESET_ALL		0x80

/* Stream ID invalid */
#define MAX96724_INVAL_ST_ID		0xFF
#define MAX96724_RESET_ST_ID		0x00
#define MAX96724_ST_ID_SEL_INVALID	0xF

/* Controller mapping: all mappings → Controller 0 */
#define MAX96724_ALL_MAP_CTRL0		0x00
/* Controller mapping: all mappings → Controller 1 */
#define MAX96724_ALL_MAP_CTRL1		0x55

/* Three mappings to controller 1 */
#define MAX96724_MAP3_CTRL1		0x15

/* ================================================================
 * Data Structures
 * ================================================================ */

/* Quad GMSL: up to 4 sources */
#define MAX96724_MAX_SOURCES		4
#define MAX96724_MAX_PIPES		4

#define MAX96724_PIPE_X			0
#define MAX96724_PIPE_Y			1
#define MAX96724_PIPE_Z			2
#define MAX96724_PIPE_U			3
#define MAX96724_PIPE_INVALID		0xF

#define MAX96724_CSI_CTRL_0		0
#define MAX96724_CSI_CTRL_1		1
#define MAX96724_CSI_CTRL_2		2
#define MAX96724_CSI_CTRL_3		3

#define MAX96724_CSI_MODE_2X4		0x08  /* enum tag for the 4-lane Port A (Ctrl1) path */

/* Lane map presets */
#define MAX96724_LANE_MAP1_2X4		0xE4  /* SC20190112 subset: PHY0 data + PHY1 clock */
#define MAX96724_LANE_MAP2_2X4		0x44  /* keep unused PHY2/3 straight in 0x08 mode */

struct max96724_source_ctx {
	struct gmsl_link_ctx *g_ctx;
	bool st_enabled;
};

struct pipe_ctx {
	u32 id;
	u32 dt_type;
	u32 dt_type2;
	u32 vc_id;
	u32 dst_csi_ctrl;
	u32 st_count;
	u32 st_id_sel;
	bool map_configured;
};

struct max96724 {
	struct i2c_client *i2c_client;
	struct regmap *regmap;
	u32 num_src;
	u32 max_src;
	u32 num_src_found;
	u32 src_link;
	bool splitter_enabled;
	struct max96724_source_ctx sources[MAX96724_MAX_SOURCES];
	struct mutex lock;
	u32 sdev_ref;
	bool lane_setup;
	bool link_setup;
	struct pipe_ctx pipe[MAX96724_MAX_PIPES];
	u8 csi_mode;
	u8 lane_mp1;
	u8 lane_mp2;
	int reset_gpio;
	int pw_ref;
	struct regulator *vdd_cam_1v2;
	u8 link_speed;  /* DT-configurable: 3 or 6 Gbps */
	bool pixel_mode;
	bool poc_enabled; /* FG24-4CH board POC/IO enable applied once */
	bool datapath_retriggered;
	u8 retriggered_pipe_mask;
};

struct reg_pair {
	u16 addr;
	u8 val;
};

/* ================================================================
 * Low-level I2C
 * ================================================================ */

static int max96724_write_reg(struct device *dev, u16 addr, u8 val)
{
	struct max96724 *priv;
	int err;

	priv = dev_get_drvdata(dev);

	err = regmap_write(priv->regmap, addr, val);
	if (err)
		dev_err(dev, "%s:i2c write failed, 0x%x = %x\n",
			__func__, addr, val);

	/* delay before next i2c command as required for SERDES link */
	usleep_range(100, 110);

	return err;
}

static int max96724_set_registers(struct device *dev, struct reg_pair *map,
				  u32 count)
{
	int err = 0;
	u32 j;

	for (j = 0; j < count; j++) {
		err = max96724_write_reg(dev, map[j].addr, map[j].val);
		if (err)
			break;
	}

	return err;
}

static const char *max96724_link_name(u32 link)
{
	switch (link) {
	case GMSL_SERDES_CSI_LINK_A:
		return "A";
	case GMSL_SERDES_CSI_LINK_B:
		return "B";
	default:
		return "?";
	}
}

static u16 max96724_link_lock_addr(u32 link)
{
	switch (link) {
	case GMSL_SERDES_CSI_LINK_A:
		return MAX96724_LINK_A_LOCK_ADDR;
	case GMSL_SERDES_CSI_LINK_B:
		return MAX96724_LINK_B_LOCK_ADDR;
	default:
		return 0;
	}
}

static void max96724_log_link_status(struct device *dev, const char *tag)
{
	struct max96724 *priv = dev_get_drvdata(dev);
	unsigned int lock_a = 0, lock_b = 0, lock_c = 0, lock_d = 0;
	unsigned int link_en = 0, reg10 = 0, reg11 = 0;

	regmap_read(priv->regmap, MAX96724_LINK_A_LOCK_ADDR, &lock_a);
	regmap_read(priv->regmap, MAX96724_LINK_B_LOCK_ADDR, &lock_b);
	regmap_read(priv->regmap, MAX96724_LINK_C_LOCK_ADDR, &lock_c);
	regmap_read(priv->regmap, MAX96724_LINK_D_LOCK_ADDR, &lock_d);
	regmap_read(priv->regmap, MAX96724_LINK_EN_ADDR, &link_en);
	regmap_read(priv->regmap, MAX96724_LINK_RATE_AB_ADDR, &reg10);
	regmap_read(priv->regmap, MAX96724_LINK_RATE_CD_ADDR, &reg11);

	dev_info(dev,
		 "GMSL link status[%s]: EN=0x%02x A@0x%04x=0x%02x(%s) B@0x%04x=0x%02x(%s) C@0x%04x=0x%02x(%s) D@0x%04x=0x%02x(%s) REG10=0x%02x REG11=0x%02x\n",
		 tag, link_en,
		 MAX96724_LINK_A_LOCK_ADDR,
		 lock_a, (lock_a & MAX96724_LINK_LOCKED) ? "LOCK" : "noLk",
		 MAX96724_LINK_B_LOCK_ADDR,
		 lock_b, (lock_b & MAX96724_LINK_LOCKED) ? "LOCK" : "noLk",
		 MAX96724_LINK_C_LOCK_ADDR,
		 lock_c, (lock_c & MAX96724_LINK_LOCKED) ? "LOCK" : "noLk",
		 MAX96724_LINK_D_LOCK_ADDR,
		 lock_d, (lock_d & MAX96724_LINK_LOCKED) ? "LOCK" : "noLk",
		 reg10, reg11);
}

void max96724_log_control_status(struct device *dev)
{
	static const u16 regs[] = {
		0x0001, 0x0003, 0x0007, 0x0006, 0x0010, 0x001A, 0x002E,
		0x00C7, 0x0500, 0x0501, 0x0503, 0x0504, 0x0506, 0x0507,
		0x0560, 0x0561, 0x0563, 0x0564, 0x0566, 0x0567, 0x0640,
		0x0641, 0x0680, 0x0681, 0x1003, 0x1004,
	};
	struct max96724 *priv = dev_get_drvdata(dev);
	unsigned int val[ARRAY_SIZE(regs)] = { 0 };
	u32 failed = 0;
	unsigned int i;

	mutex_lock(&priv->lock);
	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		if (regmap_read(priv->regmap, regs[i], &val[i]))
			failed |= BIT(i);
	}
	mutex_unlock(&priv->lock);

	dev_err(dev,
		"GMSL CC snapshot: failed=0x%08x local=0x%02x remote=0x%02x cross=0x%02x link_en=0x%02x rate=0x%02x lock=0x%02x intr11=0x%02x i2c7=0x%02x\n",
		failed, val[0], val[1], val[2], val[3], val[4], val[5],
		val[6], val[7]);
	dev_err(dev,
		"GMSL CC0/1 A: cc0 tr0=0x%02x tr1=0x%02x tr3=0x%02x tr4=0x%02x arq1=0x%02x arq2=0x%02x; cc1 tr0=0x%02x tr1=0x%02x tr3=0x%02x tr4=0x%02x arq1=0x%02x arq2=0x%02x\n",
		val[8], val[9], val[10], val[11], val[12], val[13], val[14],
		val[15], val[16], val[17], val[18], val[19]);
	dev_err(dev,
		"GMSL I2C bridge A: p0 i2c0=0x%02x i2c1=0x%02x p1 i2c0=0x%02x i2c1=0x%02x tx3=0x%02x rx0=0x%02x\n",
		val[20], val[21], val[22], val[23], val[24], val[25]);
}
EXPORT_SYMBOL(max96724_log_control_status);

static int max96724_wait_link_lock(struct device *dev, u32 link)
{
	struct max96724 *priv = dev_get_drvdata(dev);
	u16 lock_addr = max96724_link_lock_addr(link);
	unsigned int lock = 0;
	int elapsed;
	int err;

	if (!lock_addr)
		return -EINVAL;

	for (elapsed = 0;
	     elapsed <= MAX96724_LINK_LOCK_TIMEOUT_MS;
	     elapsed += MAX96724_LINK_LOCK_POLL_MS) {
		err = regmap_read(priv->regmap, lock_addr, &lock);
		if (err)
			return err;

		if (lock & MAX96724_LINK_LOCKED) {
			dev_info(dev, "GMSL Link %s locked after %d ms (reg 0x%04x=0x%02x)\n",
				 max96724_link_name(link), elapsed, lock_addr, lock);
			return 0;
		}

		if (elapsed < MAX96724_LINK_LOCK_TIMEOUT_MS)
			msleep(MAX96724_LINK_LOCK_POLL_MS);
	}

	dev_err(dev, "GMSL Link %s lock timeout after %d ms (reg 0x%04x=0x%02x)\n",
		max96724_link_name(link), MAX96724_LINK_LOCK_TIMEOUT_MS,
		lock_addr, lock);
	max96724_log_link_status(dev, "timeout");

	return -ETIMEDOUT;
}

/* ================================================================
 * Internal Helpers
 * ================================================================ */

static int max96724_get_sdev_idx(struct device *dev,
				 struct device *s_dev, unsigned int *idx)
{
	struct max96724 *priv = dev_get_drvdata(dev);
	unsigned int i;
	int err = 0;

	mutex_lock(&priv->lock);
	for (i = 0; i < priv->max_src; i++) {
		if (priv->sources[i].g_ctx->s_dev == s_dev)
			break;
	}
	if (i == priv->max_src) {
		dev_err(dev, "no sdev found\n");
		err = -EINVAL;
		goto ret;
	}

	if (idx)
		*idx = i;

ret:
	mutex_unlock(&priv->lock);
	return err;
}

static void max96724_pipes_reset(struct max96724 *priv)
{
	/*
	 * Default pipe configuration for D5xx/SC1.2:
	 * In tunnel mode, DT types are not used for filtering
	 * but we initialize them for d4xx framework compatibility.
	 */
	struct pipe_ctx pipe_defaults[] = {
		{MAX96724_PIPE_X, GMSL_CSI_DT_RAW_12,
			MAX96724_CSI_CTRL_1, 0, MAX96724_INVAL_ST_ID},
		{MAX96724_PIPE_Y, GMSL_CSI_DT_RAW_12,
			MAX96724_CSI_CTRL_1, 0, MAX96724_INVAL_ST_ID},
		{MAX96724_PIPE_Z, GMSL_CSI_DT_EMBED,
			MAX96724_CSI_CTRL_1, 0, MAX96724_INVAL_ST_ID},
		{MAX96724_PIPE_U, GMSL_CSI_DT_EMBED,
			MAX96724_CSI_CTRL_1, 0, MAX96724_INVAL_ST_ID}
	};

	memcpy(priv->pipe, pipe_defaults, sizeof(pipe_defaults));
}

static void max96724_reset_ctx(struct max96724 *priv)
{
	unsigned int i;

	priv->link_setup = false;
	priv->lane_setup = false;
	priv->num_src_found = 0;
	priv->src_link = 0;
	priv->splitter_enabled = false;
	max96724_pipes_reset(priv);
	for (i = 0; i < priv->num_src; i++)
		priv->sources[i].st_enabled = false;
}

static void max96724_apply_porta_subset(struct device *dev,
						 struct max96724 *priv)
{
	/*
	 * [XML-verified 4-lane] D-PHY Port A output, 4 data lanes.
	 * [FG24-4CH working-config verified] 0x04 = 2x4 mode so Port A
	 * maps to PHY0+PHY1 (the lanes the FG24 PCB wires to Orin).  Output is
	 * not yet enabled here (bit7 = 0); setup_streaming() asserts 0x84 after
	 * the full pipeline is configured.
	 */
	max96724_write_reg(dev, MAX96724_CSI_OUT_CFG_ADDR,
			   MAX96724_CSI_MODE_2X4_VAL);
	/*
	 * [FG24-4CH working dump] Enable ALL four CSI PHYs (0x08A2=0xF0).
	 * The working FangZhu config powers every PHY; the MAX96724 DPLL
	 * needs all PHYs powered to lock reliably (single-PHY enable was
	 * observed to never lock the output clock). Port A data still
	 * leaves on PHY0/1 in 2x4 mode; PHY2/3 are just powered.
	 */
	max96724_write_reg(dev, MAX96724_CSI_PHY_EN_ADDR,
			   MAX96724_CSI_PHY_EN_ALL);
	max96724_write_reg(dev, MAX96724_CSI_LANE_MAP1_ADDR,
			   priv->lane_mp1);
	/*
	 * [FG24-4CH working dump] LANE_MAP2 (PHY2/3) = 0xE4 straight mapping.
	 * The working config writes 0x08A4=0xE4 (not the chip default 0x44).
	 */
	max96724_write_reg(dev, MAX96724_CSI_LANE_MAP2_ADDR,
			   MAX96724_CSI_LANE_MAP_DEFAULT);
	/*
	 * [FG24-4CH board] Switch MIPI output lane polarity (P/N swap).
	 * The FG24-4CH PCB inverts the differential pairs on the DES->Orin
	 * output; without this the Orin CIL never locks the HS clock.
	 * Per FangZhu MAX4CH_FG24_POC_Enable table: 0x08A5/0x08A6 = 0x3F.
	 */
	max96724_write_reg(dev, MAX96724_CSI_PHY_POL0_ADDR,
			   MAX96724_CSI_PHY_POL_SWAP);
	max96724_write_reg(dev, MAX96724_CSI_PHY_POL1_ADDR,
			   MAX96724_CSI_PHY_POL_SWAP);
	max96724_write_reg(dev, MAX96724_LANE_CTRL0_ADDR,
			   MAX96724_LANE_CTRL_4LANE);
	max96724_write_reg(dev, MAX96724_LANE_CTRL1_ADDR,
			   MAX96724_LANE_CTRL_4LANE);
	max96724_write_reg(dev, MAX96724_LANE_CTRL2_ADDR,
			   MAX96724_LANE_CTRL_4LANE);
	max96724_write_reg(dev, MAX96724_LANE_CTRL3_ADDR,
			   MAX96724_LANE_CTRL_4LANE);
}

static inline bool max96724_is_pixel_mode(struct max96724 *priv)
{
	return priv->pixel_mode;
}

/* ================================================================
 * Power Management
 * ================================================================ */

/* [FG24-4CH board] POC (Power-over-Coax) / IO enable sequence.
 *   Per FangZhu MAX4CH_FG24_POC_Enable table. On the FG24-4CH board the
 *   MFP8 line (0x0319 bit4) and the POC IO enable (0x0001 bit5) gate the
 *   board-level output / POC path. Without these the DES PLL still locks
 *   and shows internal activity, but no clock reaches the Orin connector.
 */
#define MAX96724_POC_IO_EN_ADDR		0x0001
#define MAX96724_POC_IO_EN_VAL		0xE0  /* Enable POC IO (working dump: 0xE0) */
#define MAX96724_POC_MFP8_ADDR		0x0319
#define MAX96724_POC_MFP8_PRE		0x80  /* MFP8 pre-enable */
#define MAX96724_POC_MFP8_HIGH		0x98  /* Enable POC MFP8 High (working dump: 0x98) */

/*
 * max96724_board_poc_enable - Apply FG24-4CH POC/IO enable (one-time).
 * Mirrors the vendor MAX4CH_FG24_POC_Enable table.
 */
static void max96724_board_poc_enable(struct device *dev)
{
	max96724_write_reg(dev, MAX96724_POC_IO_EN_ADDR,
			   MAX96724_POC_IO_EN_VAL);
	max96724_write_reg(dev, MAX96724_POC_MFP8_ADDR,
			   MAX96724_POC_MFP8_PRE);
	msleep(100);
	max96724_write_reg(dev, MAX96724_POC_MFP8_ADDR,
			   MAX96724_POC_MFP8_HIGH);
	msleep(500);
}

/*
 * max96724_power_on - Power on the MAX96724 deserializer
 */
int max96724_power_on(struct device *dev)
{
	struct max96724 *priv = dev_get_drvdata(dev);
	int err = 0;

	mutex_lock(&priv->lock);
	if (priv->pw_ref == 0) {
		usleep_range(1, 2);
		if (gpio_is_valid(priv->reset_gpio))
			gpio_set_value(priv->reset_gpio, 0);

		usleep_range(30, 50);

		if (priv->vdd_cam_1v2) {
			err = regulator_enable(priv->vdd_cam_1v2);
			if (unlikely(err))
				goto ret;
		}

		usleep_range(30, 50);

		/* exit reset mode: XCLR */
		if (gpio_is_valid(priv->reset_gpio)) {
			gpio_set_value(priv->reset_gpio, 0);
			usleep_range(30, 50);
			gpio_set_value(priv->reset_gpio, 1);
			usleep_range(30, 50);
		}

		msleep(20);
	}

	priv->pw_ref++;

ret:
	mutex_unlock(&priv->lock);
	return err;
}
EXPORT_SYMBOL(max96724_power_on);

void max96724_power_off(struct device *dev)
{
	struct max96724 *priv = dev_get_drvdata(dev);

	mutex_lock(&priv->lock);
	priv->pw_ref--;

	if (priv->pw_ref < 0)
		priv->pw_ref = 0;

	if (priv->pw_ref == 0) {
		usleep_range(1, 2);
		if (gpio_is_valid(priv->reset_gpio))
			gpio_set_value(priv->reset_gpio, 0);

		if (priv->vdd_cam_1v2)
			regulator_disable(priv->vdd_cam_1v2);
	}

	mutex_unlock(&priv->lock);
}
EXPORT_SYMBOL(max96724_power_off);

/* ================================================================
 * Link Setup
 * ================================================================ */

/*
 * max96724_write_link - Configure a specific GMSL link
 *
 * [Users Guide, Link Initialization, p11-12]
 *   Register 0x0006: Link Enable and Mode Select
 *   Register 0x0010: Link Rate (Links A/B)
 *
 * The link speed is DT-configurable via the "gmsl-link-speed" property.
 */
static int max96724_write_link(struct device *dev, u32 link)
{
	struct max96724 *priv = dev_get_drvdata(dev);
	u8 link_rate_ab, link_rate_cd;
	u8 link_en_val;
	int err;

	/*
	 * [Aardvark-verified] Disable CSI output before link configuration
	 * This prevents glitches on CSI bus during GMSL link setup.
	 * Re-enabled in max96724_setup_streaming() after full pipeline config.
	 */
	max96724_write_reg(dev, MAX96724_BACKTOP_EN_ADDR, 0x00);

	/* [UG Table 3 + Tech Overview REG26]
	 * Register 0x0010:
	 *   bits[7:2] = Link rate configuration
	 *   bits[1:0] = I2C CC pass-through link select
	 *     10 = route CC via SIOA (Link A)  [tech overview: 0x02 → SIOA]
	 *     01 = route CC via SIOB (Link B)
	 *
	 * For 6G + Link A: bits[7:2]=001000, bits[1:0]=10 → 0x22
	 * For 6G + Link B: bits[7:2]=001000, bits[1:0]=01 → 0x21
	 * For 3G + Link A: bits[7:2]=000100, bits[1:0]=10 → 0x12 (? or 0x11)
	 * For 3G + Link B: bits[7:2]=000100, bits[1:0]=01 → 0x11 (? or 0x09)
	 */
	if (priv->link_speed == 6) {
		if (link == GMSL_SERDES_CSI_LINK_B) {
			link_rate_ab = 0x21; /* 6G rate + CC via Link B */
		} else {
			link_rate_ab = 0x22; /* 6G rate + CC via Link A */
		}
		link_rate_cd = 0x22; /* C/D: 6G rate (CC routing don't care) */
	} else {
		if (link == GMSL_SERDES_CSI_LINK_B) {
			link_rate_ab = 0x09; /* 3G rate + CC via Link B */
		} else {
			link_rate_ab = 0x12; /* 3G rate + CC via Link A */
		}
		link_rate_cd = 0x11; /* C/D: 3G rate */
	}

	if (link == GMSL_SERDES_CSI_LINK_A) {
		link_en_val = MAX96724_LINK_EN_SIOA;
	} else if (link == GMSL_SERDES_CSI_LINK_B) {
		link_en_val = 0xF2; /* GMSL2 all, enable Link B only */
	} else {
		dev_err(dev, "%s: invalid gmsl link\n", __func__);
		return -EINVAL;
	}

	/* [UG Table 3] Set link rates + CC routing */
	max96724_write_reg(dev, MAX96724_LINK_RATE_AB_ADDR, link_rate_ab);
	max96724_write_reg(dev, MAX96724_LINK_RATE_CD_ADDR, link_rate_cd);
	if (priv->link_speed == 6) {
		u16 errch_addr = link == GMSL_SERDES_CSI_LINK_B ?
			MAX96724_ERRCH_B_ADDR : MAX96724_ERRCH_A_ADDR;

		/* Errata #5 requires this immediately after power-up/reset. */
		max96724_write_reg(dev, errch_addr,
				   MAX96724_ERRCH_FORCE_ON);
	}

	/*
	 * [XML-verified sequence] One-shot reset applies rate configuration
	 * to the link state machine, then link enable starts training.
	 * XML order: rate → ONE-SHOT → LINK_EN → wait 1000ms.
	 */
	max96724_write_reg(dev, MAX96724_ONESHOT_ADDR,
			   MAX96724_ONESHOT_ALL);
	max96724_write_reg(dev, MAX96724_LINK_EN_ADDR, link_en_val);

	/*
	 * Poll the selected physical GMSL input until link lock before
	 * accessing the remote serializer over the control channel.
	 */
	err = max96724_wait_link_lock(dev, link);
	max96724_log_link_status(dev, err ? "after-timeout" : "after-lock");
	if (err)
		return err;

	return 0;
}

int max96724_setup_link(struct device *dev, struct device *s_dev)
{
	struct max96724 *priv = dev_get_drvdata(dev);
	int err = 0;
	unsigned int i = 0;

	err = max96724_get_sdev_idx(dev, s_dev, &i);
	if (err)
		return err;

	/*
	 * [FG24-4CH] Enable board POC / MFP8 output path BEFORE the GMSL
	 * link is brought up.  The camera draws power over coax (PoC), so
	 * without this the remote camera never powers on and the link can
	 * never lock.  NOTE: this must live here (a path d5xx actually
	 * invokes via dser_ops->setup_link) and NOT in max96724_power_on(),
	 * which ds5_gmsl_serdes_setup() deliberately skips.  Run once.
	 */
	if (!priv->poc_enabled) {
		max96724_board_poc_enable(dev);
		priv->poc_enabled = true;
	}

	mutex_lock(&priv->lock);

	if (!priv->splitter_enabled) {
		err = max96724_write_link(dev,
					  priv->sources[i].g_ctx->serdes_csi_link);
		if (err)
			goto ret;

		priv->link_setup = true;
	}

ret:
	mutex_unlock(&priv->lock);
	return err;
}
EXPORT_SYMBOL(max96724_setup_link);

/* ================================================================
 * Control Setup
 * ================================================================ */

/*
 * max96724_setup_control - Configure deserializer control pipeline
 *
 * Enables splitter mode when multiple sources are present,
 * configures tunnel mode on all active pipes, and sets up
 * the MIPI CSI-2 output.
 */
int max96724_setup_control(struct device *dev, struct device *s_dev)
{
	struct max96724 *priv = dev_get_drvdata(dev);
	int err = 0;
	unsigned int i = 0;

	static const u16 tun_en_addrs[] = {
		MAX96724_PIPE0_TUN_EN_ADDR,
		MAX96724_PIPE1_TUN_EN_ADDR,
		MAX96724_PIPE2_TUN_EN_ADDR,
		MAX96724_PIPE3_TUN_EN_ADDR,
	};

	static const u16 tun_dest_addrs[] = {
		MAX96724_PIPE0_TUN_DEST_ADDR,
		MAX96724_PIPE1_TUN_DEST_ADDR,
		MAX96724_PIPE2_TUN_DEST_ADDR,
		MAX96724_PIPE3_TUN_DEST_ADDR,
	};

	err = max96724_get_sdev_idx(dev, s_dev, &i);
	if (err)
		return err;

	mutex_lock(&priv->lock);

	if (!priv->link_setup) {
		dev_err(dev, "%s: invalid state\n", __func__);
		err = -EINVAL;
		goto error;
	}

	if (priv->sources[i].g_ctx->serdev_found) {
		priv->num_src_found++;
		priv->src_link = priv->sources[i].g_ctx->serdes_csi_link;
	}

	/* Enable splitter mode for multi-camera configurations */
	if ((priv->max_src > 1U) &&
	    (priv->num_src_found > 0U) &&
	    (priv->splitter_enabled == false)) {
		/*
		 * [UG Table 3] Multi-camera: set link rates and enable all links
		 */
		u8 link_rate = (priv->link_speed == 6) ? 0x22 : 0x11;

		max96724_write_reg(dev, MAX96724_LINK_RATE_AB_ADDR,
				   link_rate);
		max96724_write_reg(dev, MAX96724_LINK_RATE_CD_ADDR,
				   link_rate);
		max96724_write_reg(dev, MAX96724_LINK_EN_ADDR,
				   MAX96724_LINK_EN_ALL);

		/* [Errata #5] For 6Gbps: force Error Channel power-up */
		if (priv->link_speed == 6) {
			max96724_write_reg(dev, MAX96724_ERRCH_A_ADDR,
					   MAX96724_ERRCH_FORCE_ON);
			max96724_write_reg(dev, MAX96724_ERRCH_B_ADDR,
					   MAX96724_ERRCH_FORCE_ON);
			max96724_write_reg(dev, MAX96724_ERRCH_C_ADDR,
					   MAX96724_ERRCH_FORCE_ON);
			max96724_write_reg(dev, MAX96724_ERRCH_D_ADDR,
					   MAX96724_ERRCH_FORCE_ON);
		}

		priv->splitter_enabled = true;

		msleep(100);
	}

	if (max96724_is_pixel_mode(priv)) {
		for (i = 0; i < MAX96724_MAX_PIPES; i++) {
			max96724_write_reg(dev, tun_en_addrs[i],
					   MAX96724_TUN_DISABLED);
		}
		max96724_write_reg(dev, MAX96724_MIPI_CTRL_SEL_ADDR,
				   MAX96724_MIPI_CTRL_SEL_PIXEL);
		max96724_write_reg(dev, MAX96724_ONESHOT_ADDR,
				   MAX96724_ONESHOT_ALL);
		priv->sdev_ref++;
		goto maybe_reset_splitter;
	}

	/*
	 * [SC20190112] Enable tunnel mode and route to Controller 1.
	 *   - TUN_EN   (0x0936): 0x01 (TUN_EN=1)
	 *   - TUN_DEST (0x0939): 0x10 = Controller 1
	 *   CSI_OUT_CFG=0x04 (2x4 mode): Controller 1 -> PHY0+1 -> Port A -> Orin
	 */
	{
		for (i = 0; i < MAX96724_MAX_PIPES; i++) {
			max96724_write_reg(dev, tun_en_addrs[i],
					   MAX96724_TUN_EN);
			max96724_write_reg(dev, tun_dest_addrs[i],
					   MAX96724_TUN_DEST_CTRL1);
		}
	}

	/*
	 * [Errata #2] Tunnel Mode SRAM LCRC: disable ERRB for SRAM LCRC
	 * errors in tunnel mode when frame/line counters are non-zero.
	 * This prevents spurious ERRB assertions (no video corruption).
	 */
	max96724_write_reg(dev, MAX96724_SRAM_LCRC_ERR_ADDR, 0x00);

	max96724_write_reg(dev, MAX96724_ONESHOT_ADDR,
			   MAX96724_ONESHOT_ALL);

	priv->sdev_ref++;

maybe_reset_splitter:

	/* Reset splitter mode if not all devices found */
	if ((priv->sdev_ref == priv->max_src) &&
	    (priv->splitter_enabled == true) &&
	    (priv->num_src_found > 0U) &&
	    (priv->num_src_found < priv->max_src)) {
		err = max96724_write_link(dev, priv->src_link);
		if (err)
			goto error;

		priv->splitter_enabled = false;
	}

error:
	mutex_unlock(&priv->lock);
	return err;
}
EXPORT_SYMBOL(max96724_setup_control);

int max96724_reset_control(struct device *dev, struct device *s_dev)
{
	struct max96724 *priv = dev_get_drvdata(dev);
	int err = 0;

	mutex_lock(&priv->lock);
	if (!priv->sdev_ref) {
		dev_info(dev, "%s: dev is already in reset state\n", __func__);
		goto ret;
	}

	priv->sdev_ref--;
	if (priv->sdev_ref == 0) {
		max96724_reset_ctx(priv);
		max96724_write_reg(dev, MAX96724_REG13_ADDR,
				   MAX96724_SOFT_RESET);

		msleep(100);
	}

ret:
	mutex_unlock(&priv->lock);
	return err;
}
EXPORT_SYMBOL(max96724_reset_control);

/* ================================================================
 * Device Registration
 * ================================================================ */

int max96724_sdev_register(struct device *dev, struct gmsl_link_ctx *g_ctx)
{
	struct max96724 *priv = NULL;
	unsigned int i;
	int err = 0;

	if (!dev || !g_ctx || !g_ctx->s_dev) {
		dev_err(dev, "%s: invalid input params\n", __func__);
		return -EINVAL;
	}

	priv = dev_get_drvdata(dev);

	mutex_lock(&priv->lock);

	if (priv->num_src > priv->max_src) {
		dev_err(dev,
			"%s: MAX96724 inputs size exhausted\n", __func__);
		err = -ENOMEM;
		goto error;
	}

	if (priv->csi_mode == MAX96724_CSI_MODE_2X4) {
		if (!((g_ctx->csi_mode == GMSL_CSI_1X4_MODE) ||
		      (g_ctx->csi_mode == GMSL_CSI_2X4_MODE))) {
			dev_err(dev, "%s: csi mode not supported\n", __func__);
			err = -EINVAL;
			goto error;
		}
	} else {
		dev_err(dev, "%s: only csi 2x4 mode is supported\n", __func__);
		err = -EINVAL;
		goto error;
	}

	for (i = 0; i < priv->num_src; i++) {
		if (g_ctx->serdes_csi_link ==
		    priv->sources[i].g_ctx->serdes_csi_link) {
			dev_err(dev,
				"%s: serdes csi link is in use\n", __func__);
			err = -EINVAL;
			goto error;
		}
		if (g_ctx->num_csi_lanes !=
		    priv->sources[i].g_ctx->num_csi_lanes) {
			dev_err(dev,
				"%s: csi num lanes mismatch\n", __func__);
			err = -EINVAL;
			goto error;
		}
	}

	priv->sources[priv->num_src].g_ctx = g_ctx;
	priv->sources[priv->num_src].st_enabled = false;

	priv->num_src++;

error:
	mutex_unlock(&priv->lock);
	return err;
}
EXPORT_SYMBOL(max96724_sdev_register);

int max96724_sdev_unregister(struct device *dev, struct device *s_dev)
{
	struct max96724 *priv = NULL;
	int err = 0;
	unsigned int i = 0;

	if (!dev || !s_dev) {
		dev_err(dev, "%s: invalid input params\n", __func__);
		return -EINVAL;
	}

	priv = dev_get_drvdata(dev);
	mutex_lock(&priv->lock);

	if (priv->num_src == 0) {
		dev_err(dev, "%s: no source found\n", __func__);
		err = -ENODATA;
		goto error;
	}

	for (i = 0; i < priv->num_src; i++) {
		if (s_dev == priv->sources[i].g_ctx->s_dev) {
			priv->sources[i].g_ctx = NULL;
			break;
		}
	}

	if (i == priv->num_src) {
		dev_err(dev,
			"%s: requested device not found\n", __func__);
		err = -EINVAL;
		goto error;
	}
	priv->num_src--;

error:
	mutex_unlock(&priv->lock);
	return err;
}
EXPORT_SYMBOL(max96724_sdev_unregister);

/* ================================================================
 * Streaming Setup
 * ================================================================ */

/*
 * max96724_setup_streaming - Configure CSI-2 output pipeline
 *
 * In Tunnel Mode, the MAX96724 outputs the reconstructed CSI-2 stream
 * with all VCs and DTs preserved from the serializer.
 *
 * The pipeline configuration:
 *   1. Configure pipe-to-link routing
 *   2. Enable tunnel mode on active pipes
 *   3. Configure MIPI CSI-2 output lanes and PHY
 *   4. Enable BACKTOP output port
 *
 */
int max96724_setup_streaming(struct device *dev, struct device *s_dev)
{
	struct max96724 *priv = dev_get_drvdata(dev);
	struct gmsl_link_ctx *g_ctx;
	int err = 0;
	unsigned int i = 0;
	unsigned int src_idx;
	u8 vid_rx0_cfg;
	u8 vid_rx6_cfg;
	u8 st_sel_cfg;
	u8 pipe_sel0;
	u8 pipe_sel1;
	u8 dpll_cfg;

	err = max96724_get_sdev_idx(dev, s_dev, &i);
	if (err)
		return err;

	mutex_lock(&priv->lock);

	dev_info(dev, "=== setup_streaming ENTER src=%u st_enabled=%d ===\n",
		 i, priv->sources[i].st_enabled);

	if (priv->sources[i].st_enabled)
		goto ret;

	src_idx = i;
	g_ctx = priv->sources[src_idx].g_ctx;
	if (max96724_is_pixel_mode(priv)) {
		pipe_sel0 = MAX96724_PIPE_SEL0_PIXEL;
		pipe_sel1 = MAX96724_PIPE_SEL1_PIXEL;
		vid_rx0_cfg = MAX96724_VID_RX0_PIXEL_VAL;
		vid_rx6_cfg = MAX96724_VID_RX6_PIXEL_VAL;
		st_sel_cfg = 0x00;
		dpll_cfg = MAX96724_DPLL_2000MBPS;
	} else {
		pipe_sel0 = MAX96724_PIPE_SEL0_PIXEL;
		pipe_sel1 = MAX96724_PIPE_SEL1_PIXEL;
		vid_rx0_cfg = MAX96724_VID_RX0_CFG_VAL;
		vid_rx6_cfg = MAX96724_VID_RX6_CFG_VAL;
		st_sel_cfg = 0x10;
		dpll_cfg = MAX96724_DPLL_2000MBPS;
	}

	/*
	 * Pipe source routing
	 * Single cam: all pipes from Link A (0x22/0x22)
	 * Disable pipes before configuration, enable at the end
	 */
	max96724_write_reg(dev, MAX96724_PIPE_SEL0_ADDR, pipe_sel0);
	max96724_write_reg(dev, MAX96724_PIPE_SEL1_ADDR, pipe_sel1);
	/* Disable all pipes during configuration */
	max96724_write_reg(dev, MAX96724_PIPE_EN_ADDR, 0x00);

	/*
	 * Keep 0x08 mode so PHY1 remains the Port A clock master,
	 * but restore the live 2-lane subset used by the bridge board.
	 */
	max96724_apply_porta_subset(dev, priv);

	/*
	 * CSI-output DPLL data rate = 2000 Mbps
	 * on ALL FOUR controller freq registers
	 */
	if (!priv->lane_setup) {
		max96724_write_reg(dev, MAX96724_DPLL_FREQ0_ADDR,
				   dpll_cfg);
		max96724_write_reg(dev, MAX96724_DPLL_FREQ1_ADDR,
				   dpll_cfg);
		max96724_write_reg(dev, MAX96724_DPLL_FREQ2_ADDR,
				   dpll_cfg);
		max96724_write_reg(dev, MAX96724_DPLL_FREQ3_ADDR,
				   dpll_cfg);

		priv->lane_setup = true;
	}

	max96724_write_reg(dev, MAX96724_MIPI_CTRL_SEL_ADDR,
			   MAX96724_MIPI_CTRL_SEL_PIXEL);

	/*
	 * Stream ID select for all pipes
	 * 0x0933 + 0x40*pipe = stream select register
	 * Value 0x10 selects the appropriate stream for tunnel mode
	 */
	for (i = 0; i < MAX96724_MAX_PIPES; i++) {
		max96724_write_reg(dev,
				   MAX96724_PIPE_X_ST_SEL_ADDR + (0x40 * i),
				   st_sel_cfg);
	}

	/*
	 * BACKTOP: enable Controller 1 / CSI-B (0x02).
	 * In 2x4 mode, Controller 1 is master for Port A -> Orin CSI.
	 */
	max96724_write_reg(dev, MAX96724_BACKTOP_EN_ADDR,
			   MAX96724_BACKTOP_CSIB_EN);

	dev_info(dev, "=== setup_streaming: writing 0x84 (force all MIPI clocks) ===\n");

	/*
	 * Assert 0x08A0=0x84 LAST: 2x4 mode + bit[7] "force all MIPI clocks running".
	 * The clock-force keeps the D-PHY clock lane continuous so the Orin CIL can lock;
	 * written only after the full pipeline is configured.
	 */
	max96724_write_reg(dev, MAX96724_CSI_OUT_CFG_ADDR,
			   MAX96724_CSI_OUT_EN_VAL);

	/* Keep all video receivers aligned with serializer heartbeat mode. */
	for (i = 0; i < MAX96724_MAX_PIPES; i++) {
		max96724_write_reg(dev,
				   MAX96724_VID_RX0_P0_ADDR +
				   (MAX96724_VID_RX_STRIDE * i),
				   vid_rx0_cfg);
		max96724_write_reg(dev,
				   MAX96724_VID_RX6_P0_ADDR +
				   (MAX96724_VID_RX_STRIDE * i),
				   vid_rx6_cfg);
		if (max96724_is_pixel_mode(priv)) {
			max96724_write_reg(dev,
					   MAX96724_PIPE0_TUN_EN_ADDR + (0x40 * i),
					   MAX96724_TUN_DISABLED);
		}
	}

	max96724_write_reg(dev, MAX96724_ONESHOT_ADDR,
			   MAX96724_ONESHOT_ALL);

	/* Enable all 4 pipes */
	max96724_write_reg(dev, MAX96724_PIPE_EN_ADDR, MAX96724_PIPE_EN_4);

	priv->sources[src_idx].st_enabled = true;

ret:
	mutex_unlock(&priv->lock);
	return err;
}
EXPORT_SYMBOL(max96724_setup_streaming);

/* ================================================================
 * Start / Stop Streaming
 * ================================================================ */

int max96724_start_streaming(struct device *dev, struct device *s_dev)
{
	struct max96724 *priv = dev_get_drvdata(dev);
	struct gmsl_link_ctx *g_ctx;
	struct gmsl_stream *g_stream;
	int err = 0;
	unsigned int i = 0;

	err = max96724_get_sdev_idx(dev, s_dev, &i);
	if (err)
		return err;

	mutex_lock(&priv->lock);
	g_ctx = priv->sources[i].g_ctx;

	for (i = 0; i < g_ctx->num_streams; i++) {
		g_stream = &g_ctx->streams[i];

		if (g_stream->des_pipe != MAX96724_PIPE_INVALID)
			max96724_write_reg(dev, g_stream->des_pipe,
					   g_stream->st_id_sel);
	}
	mutex_unlock(&priv->lock);

	return 0;
}
EXPORT_SYMBOL(max96724_start_streaming);

int max96724_stop_streaming(struct device *dev, struct device *s_dev)
{
	struct max96724 *priv = dev_get_drvdata(dev);
	struct gmsl_link_ctx *g_ctx;
	struct gmsl_stream *g_stream;
	int err = 0;
	unsigned int i = 0;

	err = max96724_get_sdev_idx(dev, s_dev, &i);
	if (err)
		return err;

	mutex_lock(&priv->lock);
	g_ctx = priv->sources[i].g_ctx;

	for (i = 0; i < g_ctx->num_streams; i++) {
		g_stream = &g_ctx->streams[i];

		if (g_stream->des_pipe != MAX96724_PIPE_INVALID)
			max96724_write_reg(dev, g_stream->des_pipe,
					   MAX96724_RESET_ST_ID);
	}

	mutex_unlock(&priv->lock);

	return 0;
}
EXPORT_SYMBOL(max96724_stop_streaming);

/* ================================================================
 * Pipe Management
 * ================================================================ */

int max96724_get_available_pipe_id(struct device *dev, int vc_id)
{
	int i;
	int pipe_id = -ENOMEM;
	struct max96724 *priv = dev_get_drvdata(dev);

	mutex_lock(&priv->lock);
	for (i = 0; i < MAX96724_MAX_PIPES; i++) {
		if (i == vc_id && !priv->pipe[i].st_count) {
			priv->pipe[i].st_count++;
			pipe_id = i;
			break;
		}
	}
	mutex_unlock(&priv->lock);

	return pipe_id;
}
EXPORT_SYMBOL(max96724_get_available_pipe_id);

int max96724_release_pipe(struct device *dev, int pipe_id)
{
	struct max96724 *priv = dev_get_drvdata(dev);

	if (pipe_id < 0 || pipe_id >= MAX96724_MAX_PIPES)
		return -EINVAL;

	mutex_lock(&priv->lock);
	priv->pipe[pipe_id].st_count = 0;
	priv->retriggered_pipe_mask &= ~BIT(pipe_id);
	mutex_unlock(&priv->lock);

	return 0;
}
EXPORT_SYMBOL(max96724_release_pipe);

/*
 * Number of pipes currently allocated (st_count != 0).  Used by the
 * sensor driver to decide whether a link ONESHOT reset is required
 * (first pipe on an idle link) or must be avoided (additional pipe on
 * a link that is already carrying live streams).
 */
int max96724_active_pipe_count(struct device *dev)
{
	struct max96724 *priv = dev_get_drvdata(dev);
	int i;
	int cnt = 0;

	mutex_lock(&priv->lock);
	for (i = 0; i < MAX96724_MAX_PIPES; i++) {
		if (priv->pipe[i].st_count)
			cnt++;
	}
	mutex_unlock(&priv->lock);

	return cnt;
}
EXPORT_SYMBOL(max96724_active_pipe_count);

/*
 * Whether GMSL link A is currently locked.  Used by the sensor driver
 * to decide if a link ONESHOT reset is actually required at stream
 * start: a locked link forwards tunnel traffic as-is and the ONESHOT
 * would only break it (and any concurrent camera-side CSI transition
 * can race the re-lock and take the whole link down, including the
 * I2C control passthrough).
 */
int max96724_link_locked(struct device *dev)
{
	struct max96724 *priv = dev_get_drvdata(dev);
	unsigned int lock = 0;

	regmap_read(priv->regmap, MAX96724_LINK_A_LOCK_ADDR, &lock);
	return (lock & MAX96724_LINK_LOCKED) ? 1 : 0;
}
EXPORT_SYMBOL(max96724_link_locked);

int max96724_get_ser_pipe_id(struct device *dev, int dser_pipe_id, int vc_id)
{
	/* In Tunnel Mode with MAX96717, all VCs are carried in a single tunnel pipe. */
	return dser_pipe_id;
}
EXPORT_SYMBOL(max96724_get_ser_pipe_id);

void max96724_reset_oneshot(struct device *dev)
{
	struct max96724 *priv = dev_get_drvdata(dev);
	unsigned int i;
	u8 pipe_sel0;
	u8 pipe_sel1;
	u8 vid_rx0_cfg;
	u8 vid_rx6_cfg;
	u8 st_sel_cfg;
	u8 dpll_cfg;
	u16 errch_addr;
	int err;

	mutex_lock(&priv->lock);
	priv->datapath_retriggered = false;
	priv->retriggered_pipe_mask = 0;
	if (priv->splitter_enabled) {
		/* Multi-camera: reset all links */
		u8 link_rate = (priv->link_speed == 6) ? 0x22 : 0x11;

		max96724_write_reg(dev, MAX96724_LINK_RATE_AB_ADDR,
				   link_rate);
		max96724_write_reg(dev, MAX96724_LINK_RATE_CD_ADDR,
				   link_rate);
		max96724_write_reg(dev, MAX96724_LINK_EN_ADDR,
				   MAX96724_LINK_EN_ALL);
	} else {
		max96724_write_reg(dev, MAX96724_ONESHOT_ADDR,
				   MAX96724_ONESHOT_ALL);
	}
	msleep(100);

	if (max96724_is_pixel_mode(priv)) {
		pipe_sel0 = MAX96724_PIPE_SEL0_PIXEL;
		pipe_sel1 = MAX96724_PIPE_SEL1_PIXEL;
		vid_rx0_cfg = MAX96724_VID_RX0_PIXEL_VAL;
		vid_rx6_cfg = MAX96724_VID_RX6_PIXEL_VAL;
		st_sel_cfg = 0x00;
		dpll_cfg = MAX96724_DPLL_2000MBPS;
	} else {
		pipe_sel0 = MAX96724_PIPE_SEL0_PIXEL;
		pipe_sel1 = MAX96724_PIPE_SEL1_PIXEL;
		vid_rx0_cfg = MAX96724_VID_RX0_CFG_VAL;
		vid_rx6_cfg = MAX96724_VID_RX6_CFG_VAL;
		st_sel_cfg = 0x10;
		dpll_cfg = MAX96724_DPLL_2000MBPS;
	}

	/*
	 * ONESHOT resets CSI/tunnel state. Restore the full pipeline config.
	 * Mirror setup_streaming() order: disable pipes -> restore everything ->
	 * BACKTOP/0x84 -> VID_RX -> final ONESHOT to latch -> enable pipes.
	 */
	max96724_write_reg(dev, MAX96724_PIPE_EN_ADDR, 0x00);

	/* Pipe source routing: all pipes from Link A */
	max96724_write_reg(dev, MAX96724_PIPE_SEL0_ADDR, pipe_sel0);
	max96724_write_reg(dev, MAX96724_PIPE_SEL1_ADDR, pipe_sel1);

	max96724_apply_porta_subset(dev, priv);

	/*
	 * CSI-output DPLL data rate = 2000 Mbps on ALL controllers.
	 * This matches the device-tree serdes_pix_clk_hz values:
	 * 500 MHz for 16-bit modes and 1000 MHz for 8-bit modes.
	 * reset_oneshot is called by the sensor after setup_streaming, so it must
	 * mirror the same DPLL values.
	 */
	max96724_write_reg(dev, MAX96724_DPLL_FREQ0_ADDR,
			   dpll_cfg);
	max96724_write_reg(dev, MAX96724_DPLL_FREQ1_ADDR,
			   dpll_cfg);
	max96724_write_reg(dev, MAX96724_DPLL_FREQ2_ADDR,
			   dpll_cfg);
	max96724_write_reg(dev, MAX96724_DPLL_FREQ3_ADDR,
			   dpll_cfg);

	max96724_write_reg(dev, MAX96724_MIPI_CTRL_SEL_ADDR,
			   MAX96724_MIPI_CTRL_SEL_PIXEL);

	max96724_write_reg(dev, MAX96724_BACKTOP_EN_ADDR,
			   MAX96724_BACKTOP_CSIB_EN);
	for (i = 0; i < MAX96724_MAX_PIPES; i++) {
		max96724_write_reg(dev,
				   MAX96724_PIPE_X_ST_SEL_ADDR + (0x40 * i),
				   st_sel_cfg);
		max96724_write_reg(dev,
				   MAX96724_VID_RX0_P0_ADDR +
				   (MAX96724_VID_RX_STRIDE * i),
				   vid_rx0_cfg);
		max96724_write_reg(dev,
				   MAX96724_VID_RX6_P0_ADDR +
				   (MAX96724_VID_RX_STRIDE * i),
				   vid_rx6_cfg);
		max96724_write_reg(dev,
				   MAX96724_TX49_PIPE_X_ADDR + (0x40 * i),
				   0x00);
	}
	if (max96724_is_pixel_mode(priv)) {
		max96724_write_reg(dev, MAX96724_PIPE0_TUN_EN_ADDR, MAX96724_TUN_DISABLED);
		max96724_write_reg(dev, MAX96724_PIPE1_TUN_EN_ADDR, MAX96724_TUN_DISABLED);
		max96724_write_reg(dev, MAX96724_PIPE2_TUN_EN_ADDR, MAX96724_TUN_DISABLED);
		max96724_write_reg(dev, MAX96724_PIPE3_TUN_EN_ADDR, MAX96724_TUN_DISABLED);
	} else {
		/* Re-enable tunnel on all pipes (ONESHOT resets TUN_EN to 0x08=disabled) */
		max96724_write_reg(dev, MAX96724_PIPE0_TUN_EN_ADDR, MAX96724_TUN_EN);
		max96724_write_reg(dev, MAX96724_PIPE1_TUN_EN_ADDR, MAX96724_TUN_EN);
		max96724_write_reg(dev, MAX96724_PIPE2_TUN_EN_ADDR, MAX96724_TUN_EN);
		max96724_write_reg(dev, MAX96724_PIPE3_TUN_EN_ADDR, MAX96724_TUN_EN);
		/* TUN_DEST: set to Controller 1 (0x10) = Port A master in 2x4 mode */
		max96724_write_reg(dev, MAX96724_PIPE0_TUN_DEST_ADDR,
				   MAX96724_TUN_DEST_CTRL1);
		max96724_write_reg(dev, MAX96724_PIPE1_TUN_DEST_ADDR,
				   MAX96724_TUN_DEST_CTRL1);
		max96724_write_reg(dev, MAX96724_PIPE2_TUN_DEST_ADDR,
				   MAX96724_TUN_DEST_CTRL1);
		max96724_write_reg(dev, MAX96724_PIPE3_TUN_DEST_ADDR,
				   MAX96724_TUN_DEST_CTRL1);
	}
	max96724_write_reg(dev, MAX96724_PIPE_EN_ADDR,
			   MAX96724_PIPE_EN_4);

	/*
	 * Assert 0x08A0=0x84 LAST:
	 * 2x4 mode + bit[7] force all MIPI clocks running.  Must mirror
	 * setup_streaming because the sensor's oneshot path overwrites it.
	 */
	max96724_write_reg(dev, MAX96724_CSI_OUT_CFG_ADDR,
				   MAX96724_CSI_OUT_EN_VAL);

	msleep(200);

	/*
	 * ONESHOT resets the complete link PHY and data path.  Restore the
	 * 6Gbps Error Channel workaround only after that reset has settled.
	 */
	if (priv->link_speed == 6) {
		if (priv->splitter_enabled) {
			err = max96724_write_reg(dev, MAX96724_ERRCH_A_ADDR,
						 MAX96724_ERRCH_FORCE_ON);
			if (!err)
				err = max96724_write_reg(dev, MAX96724_ERRCH_B_ADDR,
							 MAX96724_ERRCH_FORCE_ON);
			if (!err)
				err = max96724_write_reg(dev, MAX96724_ERRCH_C_ADDR,
							 MAX96724_ERRCH_FORCE_ON);
			if (!err)
				err = max96724_write_reg(dev, MAX96724_ERRCH_D_ADDR,
							 MAX96724_ERRCH_FORCE_ON);
			if (err)
				dev_err(dev,
					"failed to restore GMSL Error Channels after Link reset: %d\n",
					err);
			else
				dev_info(dev,
					 "restored all GMSL Error Channels after Link reset\n");
		} else {
			errch_addr = priv->src_link == GMSL_SERDES_CSI_LINK_B ?
				MAX96724_ERRCH_B_ADDR : MAX96724_ERRCH_A_ADDR;
			err = max96724_write_reg(dev, errch_addr,
						 MAX96724_ERRCH_FORCE_ON);
			if (err)
				dev_err(dev,
					"failed to restore GMSL Error Channel after Link reset: %d\n",
					err);
			else
				dev_info(dev,
					 "restored GMSL Error Channel after Link reset (0x%04x=0x%02x)\n",
					 errch_addr, MAX96724_ERRCH_FORCE_ON);
		}
	}

	priv->datapath_retriggered = true;
	mutex_unlock(&priv->lock);
}
EXPORT_SYMBOL(max96724_reset_oneshot);

/* ================================================================
 * Init Settings / Set Pipe
 * ================================================================ */

/*
 * __max96724_set_pipe - Configure per-pipe mapping registers
 */
static int __max96724_set_pipe(struct device *dev, int pipe_id,
			       u8 data_type1, u8 data_type2, u32 vc_id)
{
	int err = 0;
	int i;
	u8 en_mapping_num = 0x0F;
	u8 all_mapping_phy = MAX96724_ALL_MAP_CTRL1;  /* Controller 1 = Port A master (2x4 mode) */
	struct max96724 *priv = dev_get_drvdata(dev);

	struct reg_pair map_pipe_control[] = {
		/* Enable 4 mappings for Pipe X */
		{MAX96724_TX11_PIPE_X_EN_ADDR, 0x0F},
		/* Map data_type1 on vc_id */
		{MAX96724_PIPE_X_SRC_0_MAP_ADDR, 0x1E},
		{MAX96724_PIPE_X_DST_0_MAP_ADDR, 0x1E},
		/* Map frame start on vc_id */
		{MAX96724_PIPE_X_SRC_1_MAP_ADDR, 0x00},
		{MAX96724_PIPE_X_DST_1_MAP_ADDR, 0x00},
		/* Map frame end on vc_id */
		{MAX96724_PIPE_X_SRC_2_MAP_ADDR, 0x01},
		{MAX96724_PIPE_X_DST_2_MAP_ADDR, 0x01},
		/* Map data_type2 on vc_id */
		{MAX96724_PIPE_X_SRC_3_MAP_ADDR, 0x12},
		{MAX96724_PIPE_X_DST_3_MAP_ADDR, 0x12},
		/* All mappings to Controller 1 (2x4 mode: Ctrl1 drives Port A) */
		{MAX96724_TX45_PIPE_X_DST_CTRL_ADDR, MAX96724_ALL_MAP_CTRL1},
		{MAX96724_TX46_PIPE_X_ADDR, MAX96724_ALL_MAP_CTRL1},
		{MAX96724_TX47_PIPE_X_ADDR, MAX96724_ALL_MAP_CTRL1},
		{MAX96724_TX48_PIPE_X_ADDR, MAX96724_ALL_MAP_CTRL1},
		/* TX49: concat disabled */
		{MAX96724_TX49_PIPE_X_ADDR, 0x00},
		/*
		 * Keep DES heartbeat handling aligned with the serializer's
		 * LIM_HEART-disabled tunnel mode to avoid persistent VID_SEQ_ERR.
		 */
		{0x0100, MAX96724_VID_RX0_CFG_VAL},
		{0x0106, MAX96724_VID_RX6_CFG_VAL},
		/*
		 * Pipe stream control (offset 0x36 per pipe block).
		 * TUN_EN = 0x01.
		 */
		{0x0936, MAX96724_TUN_EN},
	};

	/*
	 * Adjust addresses for pipe offset (stride 0x40 per pipe)
	 * Entries 0-13: TX11, SRC/DST maps, TX45-TX49 (all stride 0x40)
	 * Entries 14-15: VID_RX0/6 (stride 0x12)
	 * Entry 16: Pipe stream control (stride 0x40)
	 */
	for (i = 0; i < 14; i++)
		map_pipe_control[i].addr += 0x40 * pipe_id;

	/* VID_RX offset: 0x12 per pipe */
	map_pipe_control[14].addr += 0x12 * pipe_id;
	map_pipe_control[15].addr += 0x12 * pipe_id;

	/* Pipe stream control: stride 0x40 */
	map_pipe_control[16].addr += 0x40 * pipe_id;

	if (data_type2 == 0x0) {
		en_mapping_num = 0x07;
		all_mapping_phy = MAX96724_MAP3_CTRL1;
	}

	if (max96724_is_pixel_mode(priv)) {
		map_pipe_control[10].val = 0x00;
		map_pipe_control[11].val = 0x00;
		map_pipe_control[12].val = 0x00;
		map_pipe_control[13].val = 0x00;
		map_pipe_control[14].val = MAX96724_VID_RX0_PIXEL_VAL;
		map_pipe_control[15].val = MAX96724_VID_RX6_PIXEL_VAL;
		map_pipe_control[16].val = MAX96724_TUN_DISABLED;
	} else {
		/* no concatenation needed */
		map_pipe_control[13].val = 0x00;
	}

	map_pipe_control[0].val = en_mapping_num;
	/* SRC: match incoming VC from camera (vc_id) */
	map_pipe_control[1].val = (vc_id << 6) | data_type1;
	/* DST: preserve VC — NVCSI/VI endpoints expect matching vc-id */
	map_pipe_control[2].val = (vc_id << 6) | data_type1;
	map_pipe_control[3].val = (vc_id << 6) | 0x00;
	map_pipe_control[4].val = (vc_id << 6) | 0x00;
	map_pipe_control[5].val = (vc_id << 6) | 0x01;
	map_pipe_control[6].val = (vc_id << 6) | 0x01;
	map_pipe_control[7].val = (vc_id << 6) | data_type2;
	map_pipe_control[8].val = (vc_id << 6) | data_type2;
	map_pipe_control[9].val = all_mapping_phy;
	/* TX46-TX48 keep 0x55 (Ctrl1) from initializer */
	/* TX49 set to 0x00 above — overridden per mode in pixel/tunnel branch */
	/* VID_RX and pipe stream ctrl keep init values */

	err = max96724_set_registers(dev, map_pipe_control,
				     ARRAY_SIZE(map_pipe_control));
	if (!err) {
		priv->pipe[pipe_id].dt_type = data_type1;
		priv->pipe[pipe_id].dt_type2 = data_type2;
		priv->pipe[pipe_id].vc_id = vc_id;
		priv->pipe[pipe_id].map_configured = true;
	}

	return err;
}

void max96724_retrigger_datapath(struct device *dev)
{
	struct max96724 *priv = dev_get_drvdata(dev);
	unsigned int pipe_en = MAX96724_PIPE_EN_4;
	u8 active_mask = 0;
	u8 retrigger_mask;
	u8 pipe_sel0;
	u8 pipe_sel1;
	u8 vid_rx0_cfg;
	u8 vid_rx6_cfg;
	u8 st_sel_cfg;
	u8 dpll_cfg;
	bool full_retrigger;
	unsigned int i;
	int err = 0;

	mutex_lock(&priv->lock);
	for (i = 0; i < MAX96724_MAX_PIPES; i++) {
		if (priv->pipe[i].st_count)
			active_mask |= BIT(i);
	}
	if (!active_mask) {
		dev_warn(dev, "datapath retrigger requested with no active pipes\n");
		goto out;
	}

	full_retrigger = !priv->datapath_retriggered;
	retrigger_mask = active_mask & ~priv->retriggered_pipe_mask;
	if (!full_retrigger && !retrigger_mask) {
		dev_info(dev, "CSI datapath already retriggered for active pipes 0x%02x\n",
			 active_mask);
		goto out;
	}
	if (!full_retrigger && priv->retriggered_pipe_mask) {
		/* All logical pipes on Link A share one tunnel detector. Once the
		 * first stream has armed it, toggling any PIPE_EN bit can interrupt
		 * VCs that are already live. set_pipe() has already installed the
		 * joining stream's mapping, so only track it here. */
		dev_info(dev,
			 "Link A tunnel already active; adding CSI pipes 0x%02x without toggling live pipes 0x%02x\n",
			 retrigger_mask, priv->retriggered_pipe_mask);
		priv->retriggered_pipe_mask |= retrigger_mask;
		goto out;
	}
	if (full_retrigger)
		retrigger_mask = active_mask;

	if (full_retrigger) {
		if (max96724_is_pixel_mode(priv)) {
			pipe_sel0 = MAX96724_PIPE_SEL0_PIXEL;
			pipe_sel1 = MAX96724_PIPE_SEL1_PIXEL;
			vid_rx0_cfg = MAX96724_VID_RX0_PIXEL_VAL;
			vid_rx6_cfg = MAX96724_VID_RX6_PIXEL_VAL;
			st_sel_cfg = 0x00;
			dpll_cfg = MAX96724_DPLL_2000MBPS;
		} else {
			pipe_sel0 = MAX96724_PIPE_SEL0_PIXEL;
			pipe_sel1 = MAX96724_PIPE_SEL1_PIXEL;
			vid_rx0_cfg = MAX96724_VID_RX0_CFG_VAL;
			vid_rx6_cfg = MAX96724_VID_RX6_CFG_VAL;
			st_sel_cfg = 0x10;
			dpll_cfg = MAX96724_DPLL_2000MBPS;
		}

		dev_info(dev,
			 "retriggering full CSI datapath for first stream batch\n");
		err = max96724_write_reg(dev, MAX96724_PIPE_EN_ADDR, 0x00);
		msleep(20);
		err |= max96724_write_reg(dev, MAX96724_PIPE_SEL0_ADDR,
					  pipe_sel0);
		err |= max96724_write_reg(dev, MAX96724_PIPE_SEL1_ADDR,
					  pipe_sel1);
		max96724_apply_porta_subset(dev, priv);
		err |= max96724_write_reg(dev, MAX96724_DPLL_FREQ0_ADDR,
					  dpll_cfg);
		err |= max96724_write_reg(dev, MAX96724_DPLL_FREQ1_ADDR,
					  dpll_cfg);
		err |= max96724_write_reg(dev, MAX96724_DPLL_FREQ2_ADDR,
					  dpll_cfg);
		err |= max96724_write_reg(dev, MAX96724_DPLL_FREQ3_ADDR,
					  dpll_cfg);
		err |= max96724_write_reg(dev, MAX96724_MIPI_CTRL_SEL_ADDR,
					  MAX96724_MIPI_CTRL_SEL_PIXEL);
		err |= max96724_write_reg(dev, MAX96724_BACKTOP_EN_ADDR,
					  MAX96724_BACKTOP_CSIB_EN);
		for (i = 0; i < MAX96724_MAX_PIPES; i++) {
			u16 pipe_off = 0x40 * i;
			u16 vid_off = MAX96724_VID_RX_STRIDE * i;

			err |= max96724_write_reg(dev,
				MAX96724_PIPE_X_ST_SEL_ADDR + pipe_off,
				st_sel_cfg);
			err |= max96724_write_reg(dev,
				MAX96724_VID_RX0_P0_ADDR + vid_off,
				vid_rx0_cfg);
			err |= max96724_write_reg(dev,
				MAX96724_VID_RX6_P0_ADDR + vid_off,
				vid_rx6_cfg);
			err |= max96724_write_reg(dev,
				MAX96724_TX49_PIPE_X_ADDR + pipe_off, 0x00);
			if (max96724_is_pixel_mode(priv)) {
				err |= max96724_write_reg(dev,
					MAX96724_PIPE0_TUN_EN_ADDR + pipe_off,
					MAX96724_TUN_DISABLED);
				err |= max96724_write_reg(dev,
					MAX96724_TX45_PIPE_X_DST_CTRL_ADDR + pipe_off,
					MAX96724_ALL_MAP_CTRL0);
				err |= max96724_write_reg(dev,
					MAX96724_TX46_PIPE_X_ADDR + pipe_off,
					MAX96724_ALL_MAP_CTRL0);
				err |= max96724_write_reg(dev,
					MAX96724_TX47_PIPE_X_ADDR + pipe_off,
					MAX96724_ALL_MAP_CTRL0);
				err |= max96724_write_reg(dev,
					MAX96724_TX48_PIPE_X_ADDR + pipe_off,
					MAX96724_ALL_MAP_CTRL0);
			} else {
				err |= max96724_write_reg(dev,
					MAX96724_PIPE0_TUN_EN_ADDR + pipe_off,
					MAX96724_TUN_EN);
				err |= max96724_write_reg(dev,
					MAX96724_PIPE0_TUN_DEST_ADDR + pipe_off,
					MAX96724_TUN_DEST_CTRL1);
				err |= max96724_write_reg(dev,
					MAX96724_TX45_PIPE_X_DST_CTRL_ADDR + pipe_off,
					MAX96724_ALL_MAP_CTRL1);
				err |= max96724_write_reg(dev,
					MAX96724_TX46_PIPE_X_ADDR + pipe_off,
					MAX96724_ALL_MAP_CTRL1);
				err |= max96724_write_reg(dev,
					MAX96724_TX47_PIPE_X_ADDR + pipe_off,
					MAX96724_ALL_MAP_CTRL1);
				err |= max96724_write_reg(dev,
					MAX96724_TX48_PIPE_X_ADDR + pipe_off,
					MAX96724_ALL_MAP_CTRL1);
			}
		}
	} else {
		if (regmap_read(priv->regmap, MAX96724_PIPE_EN_ADDR, &pipe_en))
			pipe_en = MAX96724_PIPE_EN_4;
		dev_info(dev,
			 "retriggering newly active CSI pipes 0x%02x (active 0x%02x)\n",
			 retrigger_mask, active_mask);
		err = max96724_write_reg(dev, MAX96724_PIPE_EN_ADDR,
					 pipe_en & ~retrigger_mask);
		msleep(20);
	}

	for (i = 0; i < MAX96724_MAX_PIPES; i++) {
		struct pipe_ctx *pipe = &priv->pipe[i];

		if (!(retrigger_mask & BIT(i)) || !pipe->st_count ||
		    !pipe->map_configured)
			continue;
		dev_info(dev,
			 "re-applying pipe %u mapping dt1 0x%x dt2 0x%x vc %u\n",
			 i, pipe->dt_type, pipe->dt_type2, pipe->vc_id);
		err |= __max96724_set_pipe(dev, i, pipe->dt_type,
					   pipe->dt_type2, pipe->vc_id);
	}
	if (err)
		dev_warn(dev, "failed to re-apply active pipe mappings: %d\n",
			 err);

	err |= max96724_write_reg(dev, MAX96724_CSI_OUT_CFG_ADDR,
				  MAX96724_CSI_OUT_EN_VAL);
	if (full_retrigger) {
		err |= max96724_write_reg(dev, MAX96724_PIPE_EN_ADDR,
					  MAX96724_PIPE_EN_4);
		if (!err)
			priv->datapath_retriggered = true;
	} else {
		err |= max96724_write_reg(dev, MAX96724_PIPE_EN_ADDR,
					  pipe_en | retrigger_mask);
	}
	if (err) {
		dev_warn(dev, "failed to retrigger CSI datapath: %d\n", err);
	} else {
		priv->retriggered_pipe_mask |= retrigger_mask;
	}

out:
	mutex_unlock(&priv->lock);
}
EXPORT_SYMBOL(max96724_retrigger_datapath);

/*
 * max96724_init_settings - Initialize default pipe and tunnel configuration
 *
 * Sets up all 4 pipes with default YUV422_8 + EMBED for VC0-3,
 * matching the d4xx framework expectations.
 *
 */
int max96724_init_settings(struct device *dev)
{
	int err = 0;
	int i;
	struct max96724 *priv = dev_get_drvdata(dev);

	mutex_lock(&priv->lock);

	for (i = 0; i < MAX96724_MAX_PIPES; i++)
		err |= __max96724_set_pipe(dev, i, GMSL_CSI_DT_YUV422_8,
					   GMSL_CSI_DT_EMBED, i);

	mutex_unlock(&priv->lock);

	return err;
}
EXPORT_SYMBOL(max96724_init_settings);

int max96724_bind_ser_to_dser_pipe(struct device *dev, int dser_pipe_id,
				   int ser_pipe_id, u32 vc_id)
{
	/* In Tunnel Mode, pipe mapping is 1:1 (each camera → one pipe) */
	return 0;
}
EXPORT_SYMBOL(max96724_bind_ser_to_dser_pipe);

int max96724_set_pipe(struct device *dev, int pipe_id,
		      u8 data_type1, u8 data_type2, u32 vc_id)
{
	struct max96724 *priv = dev_get_drvdata(dev);
	int err = 0;

	if (pipe_id > (MAX96724_MAX_PIPES - 1)) {
		dev_info(dev, "%s: input pipe_id: %d exceeds max96724 max pipes\n",
			 __func__, pipe_id);
		return -EINVAL;
	}

	dev_dbg(dev, "%s pipe_id %d, data_type1 %u, data_type2 %u, vc_id %u\n",
		__func__, pipe_id, data_type1, data_type2, vc_id);

	mutex_lock(&priv->lock);

	err = __max96724_set_pipe(dev, pipe_id, data_type1, data_type2, vc_id);

	mutex_unlock(&priv->lock);

	return err;
}
EXPORT_SYMBOL(max96724_set_pipe);

/* ================================================================
 * Frame Sync (FSYNC) — internal generator, broadcast to all links
 * ================================================================ */

/*
 * Multi-camera frame synchronization.
 *
 * The MAX96724 generates the FSYNC signal internally from its on-board
 * 25MHz crystal and broadcasts it over the GMSL2 reverse channel to
 * every serializer on GPIO channel 23.  Each MAX96717 already receives
 * ch23 on GPIO0 (H_VSYNC_TRIG) and GPIO1 (RGB_FSYNC) — see
 * max96717_setup_gpio_tunneling() — so all cameras are driven by the
 * same deserializer-generated edge with zero inter-camera skew.  No
 * camera strobe loop-back and no SoC sync pin are required.
 *
 * Register sequence validated against the MAX96724 User Guide
 * "Internal FSYNC (GMSL2)" programming example.
 */
#define MAX96724_FSYNC_0_ADDR		0x04A0	/* mode / method / enable      */
#define MAX96724_FSYNC_PER_L_ADDR	0x04A5	/* FSYNC_PERIOD[7:0]           */
#define MAX96724_FSYNC_PER_M_ADDR	0x04A6	/* FSYNC_PERIOD[15:8]          */
#define MAX96724_FSYNC_PER_H_ADDR	0x04A7	/* FSYNC_PERIOD[23:16]         */
#define MAX96724_FSYNC_15_ADDR		0x04AF	/* link select / XTAL time base*/
#define MAX96724_FSYNC_TXID_ADDR	0x04B1	/* FSYNC_TX_ID[7:3]            */

/* FSYNC time base = on-board 25MHz crystal. */
#define MAX96724_FSYNC_XTAL_HZ		25000000U
/* Fallback frame rate (Hz) if the caller passes fps == 0 (unknown). */
#define MAX96724_FSYNC_FPS_DEFAULT	30U
/* FSYNC_PERIOD is a 24-bit field (0x04A5..0x04A7). */
#define MAX96724_FSYNC_PERIOD_MAX	0x00FFFFFFU

/* GMSL2 GPIO channel the serializers receive sync on
 * (matches MAX96717 GPIO0/GPIO1 RX_ID=23). */
#define MAX96724_FSYNC_TX_CH		23

/*
 * 0x04AF: GMSL2-type FSYNC output (bit7=1), use 25MHz XTAL as the time
 * base (bit6=1), select links via FS_LINK_x rather than "all enabled"
 * (bit4=0), and include all four video pipes (bits[3:0]=1111).
 */
#define MAX96724_FSYNC_15_ALL		0xCF
/* 0x04B1: FSYNC_TX_ID in bits[7:3]; FSYNC_ERR_THR[2:0] left at 0. */
#define MAX96724_FSYNC_TXID_VAL		(MAX96724_FSYNC_TX_CH << 3)
/*
 * 0x04A0: FSYNC_MODE=01 (master deserializer — drives subordinates over
 * the GMSL reverse channel) | FSYNC_METH=00 (manual / internal period).
 * Must be written last to start the FSYNC state machine.
 */
#define MAX96724_FSYNC_0_ENABLE		0x04
/* 0x04A0 disable: FSYNC_MODE=11 (off / power-up reset state). */
#define MAX96724_FSYNC_0_DISABLE	0x0C

/*
 * max96724_setup_fsync - enable internal FSYNC broadcast (multi-camera sync)
 * @dev: deserializer device handle
 * @fps: desired frame rate in Hz, normally the V4L2-negotiated value
 *       (VIDIOC_S_PARM / frame interval); pass 0 to use the default.
 *
 * Programs the MAX96724 as the FSYNC master: it generates a frame-sync
 * pulse internally from the 25MHz crystal and transmits it on GMSL GPIO
 * channel 23 to all four links.  Every camera's serializer receives the
 * same edge (via max96717_setup_gpio_tunneling()), giving frame-locked,
 * skew-free multi-camera capture.  The FSYNC period is computed at
 * runtime as XTAL / fps so the sync rate tracks the active V4L2 mode.
 */
int max96724_setup_fsync(struct device *dev, u32 fps)
{
	struct max96724 *priv = dev_get_drvdata(dev);
	struct reg_pair map[6];
	u32 period;
	int err;

	if (!fps)
		fps = MAX96724_FSYNC_FPS_DEFAULT;

	/* Convert frame rate to a 24-bit period in 25MHz XTAL cycles. */
	period = MAX96724_FSYNC_XTAL_HZ / fps;
	if (!period || period > MAX96724_FSYNC_PERIOD_MAX) {
		dev_err(dev, "%s: fps %u out of range (period %u)\n",
			__func__, fps, period);
		return -EINVAL;
	}

	/* Link select + 25MHz XTAL time base (all 4 links). */
	map[0].addr = MAX96724_FSYNC_15_ADDR;
	map[0].val  = MAX96724_FSYNC_15_ALL;
	/* FSYNC period (24-bit, in XTAL cycles). */
	map[1].addr = MAX96724_FSYNC_PER_L_ADDR;
	map[1].val  = period & 0xFF;
	map[2].addr = MAX96724_FSYNC_PER_M_ADDR;
	map[2].val  = (period >> 8) & 0xFF;
	map[3].addr = MAX96724_FSYNC_PER_H_ADDR;
	map[3].val  = (period >> 16) & 0xFF;
	/* Transmit FSYNC over GMSL reverse channel 23 to all SERs. */
	map[4].addr = MAX96724_FSYNC_TXID_ADDR;
	map[4].val  = MAX96724_FSYNC_TXID_VAL;
	/* Enable internal FSYNC, manual mode (must be written last). */
	map[5].addr = MAX96724_FSYNC_0_ADDR;
	map[5].val  = MAX96724_FSYNC_0_ENABLE;

	mutex_lock(&priv->lock);

	err = max96724_set_registers(dev, map, ARRAY_SIZE(map));
	if (err)
		dev_err(dev, "%s: FSYNC setup failed (%d)\n", __func__, err);
	else
		dev_info(dev,
			 "%s: internal FSYNC enabled (%u Hz, period %u, broadcast ch%u to all links)\n",
			 __func__, fps, period, MAX96724_FSYNC_TX_CH);

	mutex_unlock(&priv->lock);

	return err;
}
EXPORT_SYMBOL(max96724_setup_fsync);

/*
 * max96724_disable_fsync - disable internal FSYNC generation
 *
 * Turns the FSYNC state machine off (FSYNC_MODE=11/off).  Counterpart to
 * max96724_setup_fsync().
 */
int max96724_disable_fsync(struct device *dev)
{
	struct max96724 *priv = dev_get_drvdata(dev);
	struct reg_pair map[] = {
		{MAX96724_FSYNC_0_ADDR, MAX96724_FSYNC_0_DISABLE},
	};
	int err;

	mutex_lock(&priv->lock);

	err = max96724_set_registers(dev, map, ARRAY_SIZE(map));
	if (err)
		dev_err(dev, "%s: FSYNC disable failed (%d)\n", __func__, err);
	else
		dev_dbg(dev, "%s: internal FSYNC disabled\n", __func__);

	mutex_unlock(&priv->lock);

	return err;
}
EXPORT_SYMBOL(max96724_disable_fsync);

/* ================================================================
 * Device Tree Parsing
 * ================================================================ */

static const struct of_device_id max96724_of_match[] = {
	{ .compatible = "adi,max96724", },
	{ },
};
MODULE_DEVICE_TABLE(of, max96724_of_match);

static int max96724_parse_dt(struct max96724 *priv,
			     struct i2c_client *client)
{
	struct device_node *node = client->dev.of_node;
	int err = 0;
	const char *str_value;
	int value;
	const struct of_device_id *match;

	if (!node)
		return -EINVAL;

	match = of_match_device(max96724_of_match, &client->dev);
	if (!match) {
		dev_err(&client->dev, "Failed to find matching dt id\n");
		return -EFAULT;
	}

	/* CSI mode: "2x4" or "4x2" */
	err = of_property_read_string(node, "csi-mode", &str_value);
	if (err < 0) {
		dev_err(&client->dev, "csi-mode property not found\n");
		return err;
	}

	if (!strcmp(str_value, "2x4")) {
		priv->csi_mode = MAX96724_CSI_MODE_2X4;
		priv->lane_mp1 = MAX96724_LANE_MAP1_2X4;
		priv->lane_mp2 = MAX96724_LANE_MAP2_2X4;
	} else {
		dev_err(&client->dev, "invalid csi mode\n");
		return -EINVAL;
	}

	/* Max sources */
	err = of_property_read_u32(node, "max-src", &value);
	if (err < 0) {
		dev_err(&client->dev, "No max-src info\n");
		return err;
	}
	priv->max_src = value;

	/* Reset GPIO */
	priv->reset_gpio = of_get_named_gpio(node, "reset-gpios", 0);
	if (priv->reset_gpio < 0) {
		dev_info(&client->dev,
			 "reset-gpios not found, continuing without external reset\n");
		priv->reset_gpio = -1;
	}

	/* GMSL link speed from DT: 3 or 6 (Gbps) */
	err = of_property_read_u32(node, "gmsl-link-speed", &value);
	if (err < 0) {
		/* Default to 3 Gbps if not specified */
		priv->link_speed = 3;
		dev_info(&client->dev,
			 "gmsl-link-speed not specified, defaulting to 3 Gbps\n");
	} else {
		if (value != 3 && value != 6) {
			dev_err(&client->dev,
				"invalid gmsl-link-speed %d (must be 3 or 6)\n",
				value);
			return -EINVAL;
		}
		priv->link_speed = value;
	}

	priv->pixel_mode = of_property_read_bool(node, "adi,pixel-mode");

	/* 1.2V regulator (optional) */
	if (of_get_property(node, "vdd_cam_1v2-supply", NULL)) {
		priv->vdd_cam_1v2 = regulator_get(&client->dev, "vdd_cam_1v2");
		if (IS_ERR(priv->vdd_cam_1v2)) {
			dev_err(&client->dev,
				"vdd_cam_1v2 regulator get failed\n");
			err = PTR_ERR(priv->vdd_cam_1v2);
			priv->vdd_cam_1v2 = NULL;
			return err;
		}
	} else {
		priv->vdd_cam_1v2 = NULL;
	}

	return 0;
}

/* ================================================================
 * Probe / Remove
 * ================================================================ */

static struct regmap_config max96724_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.cache_type = REGCACHE_NONE,
};

static int max96724_probe(struct i2c_client *client,
			   const struct i2c_device_id *id)
{
	struct max96724 *priv;
	int err = 0;

	dev_info(&client->dev,
		 "[MAX96724]: probing Quad GMSL2 Deserializer (Tunnel Mode)\n");

	priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->i2c_client = client;
	priv->regmap = devm_regmap_init_i2c(priv->i2c_client,
					     &max96724_regmap_config);
	if (IS_ERR(priv->regmap)) {
		dev_err(&client->dev,
			"regmap init failed: %ld\n", PTR_ERR(priv->regmap));
		return -ENODEV;
	}

	err = max96724_parse_dt(priv, client);
	if (err) {
		dev_err(&client->dev, "unable to parse dt\n");
		return -EFAULT;
	}

	max96724_pipes_reset(priv);

	if (priv->max_src > MAX96724_MAX_SOURCES) {
		dev_err(&client->dev,
			"max sources more than currently supported\n");
		return -EINVAL;
	}

	mutex_init(&priv->lock);

	dev_set_drvdata(&client->dev, priv);

	dev_info(&client->dev, "%s: success (link_speed=%d Gbps, max_src=%d)\n",
		 __func__, priv->link_speed, priv->max_src);

	return err;
}

static int max96724_remove(struct i2c_client *client)
{
	struct max96724 *priv = dev_get_drvdata(&client->dev);

	mutex_destroy(&priv->lock);

	return 0;
}

static const struct i2c_device_id max96724_id[] = {
	{ "max96724", 0 },
	{ },
};

MODULE_DEVICE_TABLE(i2c, max96724_id);

static struct i2c_driver max96724_i2c_driver = {
	.driver = {
		.name = "max96724",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(max96724_of_match),
	},
	.probe = max96724_probe,
	.remove = max96724_remove,
	.id_table = max96724_id,
};

static int __init max96724_init(void)
{
	return i2c_add_driver(&max96724_i2c_driver);
}

static void __exit max96724_exit(void)
{
	i2c_del_driver(&max96724_i2c_driver);
}

module_init(max96724_init);
module_exit(max96724_exit);

MODULE_DESCRIPTION("Quad GMSL2 Deserializer driver max96724 (Tunnel Mode)");
MODULE_AUTHOR("RealSense AI");
MODULE_LICENSE("GPL v2");
