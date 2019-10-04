// SPDX-License-Identifier: GPL-2.0
/*
 * Description   : JTAG Master driver
 *
 * Copyright (C) 2019 NuvoTon Corporation
 *
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/clk.h>
#include <linux/uaccess.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include <linux/cdev.h>
#include <linux/miscdevice.h>
#include <linux/interrupt.h>
#include "jtag_drv.h"

#ifdef _JTAG_DEBUG
#define JTAG_DBUG(fmt, args...) \
		pr_debug("%s() " fmt, __func__, ## args)
#else
#define JTAG_DBUG(fmt, args...)
#endif
/* GPIO Port Registers */
#define GPnDIN	0x04	/* Data In */
#define GPnDOUT	0x0C	/* Data Out */
#define GPnDOS	0x68	/* Data Out Set */
#define GPnDOC	0x6C	/* Data Out Clear */

#define high			1
#define low				0

/* default jtag speed in MHz */
#define JTAG_PSPI_SPEED		(10 * 1000000)
#define JTAG_PSPI_MAX_FREQ	(25 * 1000000)

#define PSPI1	1
#define PSPI2	2
/* Multiple Function Pin Selection */
#define MFSEL3_OFFSET 0x064
#define PSPI1SEL_OFFSET	3
#define PSPI1SEL_MASK	3
#define PSPI1SEL_GPIO	0
#define PSPI1SEL_PSPI	2
#define PSPI2SEL_OFFSET	13
#define PSPI2SEL_MASK	1
#define PSPI2SEL_GPIO	0
#define PSPI2SEL_PSPI	1

/* PSPI registers */
#define PSPI_DATA		0x00
#define PSPI_CTL1		0x02
#define PSPI_STAT		0x04

#define PSPI_CTL1_SCDV6_0	9
#define PSPI_CTL1_SCIDL		8
#define PSPI_CTL1_SCM		7
#define PSPI_CTL1_EIW		6
#define PSPI_CTL1_EIR		5
#define PSPI_CTL1_SPIEN		0

#define PSPI_STAT_RBF		1
#define PSPI_STAT_BSY		0

#define BIT_MODE_8	1
#define BIT_MODE_16	2
static unsigned char reverse[16] = {
	0x0, 0x8, 0x4, 0xC, 0x2, 0xA, 0x6, 0xE,
	0x1, 0x9, 0x5, 0xD, 0x3, 0xB, 0x7, 0xF
};
#define REVERSE(x)  ((reverse[(x & 0x0f)] << 4) | reverse[(x & 0xf0) >> 4])

static DEFINE_SPINLOCK(jtag_file_lock);

struct jtag_pins {
	struct gpio_desc *gpiod;
	void __iomem *reg_base;
	unsigned int gpio;
	int bit_offset;
};

struct npcm_pspi {
	struct device *dev;
	struct completion xfer_done;
	void __iomem *base;
	spinlock_t lock;
	u32 apb_clk_rate;
	bool enable_irq;
	int mode;
	char *tx_buf;
	char *rx_buf;
	unsigned int tx_bytes;
	unsigned int rx_bytes;
};

struct jtag_info {
	struct device *dev;
	struct miscdevice miscdev;
	struct npcm_pspi pspi;
	struct jtag_pins pins[pin_NUM];
	struct regmap		*gcr_regmap;
	u32 freq;
	u32 controller; /* PSPI controller */
	u8 tms_level;
	u8 tapstate;
	bool is_open;

	/* transmit tck/tdi/tdo by pspi */
	#define MODE_PSPI 		0
	/* transmit all signals by gpio */
	#define MODE_GPIO 		1
	u8 mode;

	/* control gpio by register directly */
	#define GPIOCTRL_DIRECT	0
	/* control gpio by gpiolib api */
	#define GPIOCTRL_API		1
	u8 gpio_ctrl;
};

/* this structure represents a TMS cycle, as expressed in a set of bits and
 * a count of bits (note: there are no start->end state transitions that
 * require more than 1 byte of TMS cycles)
 */
struct TmsCycle {
	unsigned char tmsbits;
	unsigned char count;
};

/* this is the complete set TMS cycles for going from any TAP state to
 * any other TAP state, following a “shortest path” rule
 */
const struct TmsCycle _tmsCycleLookup[][16] = {
/*      TLR        RTI       SelDR      CapDR      SDR      */
/*      Ex1DR      PDR       Ex2DR      UpdDR      SelIR    */
/*      CapIR      SIR       Ex1IR      PIR        Ex2IR    */
/*      UpdIR                                               */
/* TLR */
	{
		{0x01, 1}, {0x00, 1}, {0x02, 2}, {0x02, 3}, {0x02, 4},
		{0x0a, 4}, {0x0a, 5}, {0x2a, 6}, {0x1a, 5}, {0x06, 3},
		{0x06, 4}, {0x06, 5}, {0x16, 5}, {0x16, 6}, {0x56, 7},
		{0x36, 6}
	},
/* RTI */
	{
		{0x07, 3}, {0x00, 1}, {0x01, 1}, {0x01, 2}, {0x01, 3},
		{0x05, 3}, {0x05, 4}, {0x15, 5}, {0x0d, 4}, {0x03, 2},
		{0x03, 3}, {0x03, 4}, {0x0b, 4}, {0x0b, 5}, {0x2b, 6},
		{0x1b, 5}
	},
/* SelDR */
	{
		{0x03, 2}, {0x03, 3}, {0x00, 0}, {0x00, 1}, {0x00, 2},
		{0x02, 2}, {0x02, 3}, {0x0a, 4}, {0x06, 3}, {0x01, 1},
		{0x01, 2}, {0x01, 3}, {0x05, 3}, {0x05, 4}, {0x15, 5},
		{0x0d, 4}
	},
/* CapDR */
	{
		{0x1f, 5}, {0x03, 3}, {0x07, 3}, {0x00, 0}, {0x00, 1},
		{0x01, 1}, {0x01, 2}, {0x05, 3}, {0x03, 2}, {0x0f, 4},
		{0x0f, 5}, {0x0f, 6}, {0x2f, 6}, {0x2f, 7}, {0xaf, 8},
		{0x6f, 7}
	},
/* SDR */
	{
		{0x1f, 5}, {0x03, 3}, {0x07, 3}, {0x07, 4}, {0x00, 0},
		{0x01, 1}, {0x01, 2}, {0x05, 3}, {0x03, 2}, {0x0f, 4},
		{0x0f, 5}, {0x0f, 6}, {0x2f, 6}, {0x2f, 7}, {0xaf, 8},
		{0x6f, 7}
	},
/* Ex1DR */
	{
		{0x0f, 4}, {0x01, 2}, {0x03, 2}, {0x03, 3}, {0x02, 3},
		{0x00, 0}, {0x00, 1}, {0x02, 2}, {0x01, 1}, {0x07, 3},
		{0x07, 4}, {0x07, 5}, {0x17, 5}, {0x17, 6}, {0x57, 7},
		{0x37, 6}
	},
/* PDR */
	{
		{0x1f, 5}, {0x03, 3}, {0x07, 3}, {0x07, 4}, {0x01, 2},
		{0x05, 3}, {0x00, 1}, {0x01, 1}, {0x03, 2}, {0x0f, 4},
		{0x0f, 5}, {0x0f, 6}, {0x2f, 6}, {0x2f, 7}, {0xaf, 8},
		{0x6f, 7}
	},
/* Ex2DR */
	{
		{0x0f, 4}, {0x01, 2}, {0x03, 2}, {0x03, 3}, {0x00, 1},
		{0x02, 2}, {0x02, 3}, {0x00, 0}, {0x01, 1}, {0x07, 3},
		{0x07, 4}, {0x07, 5}, {0x17, 5}, {0x17, 6}, {0x57, 7},
		{0x37, 6}
	},
/* UpdDR */
	{
		{0x07, 3}, {0x00, 1}, {0x01, 1}, {0x01, 2}, {0x01, 3},
		{0x05, 3}, {0x05, 4}, {0x15, 5}, {0x00, 0}, {0x03, 2},
		{0x03, 3}, {0x03, 4}, {0x0b, 4}, {0x0b, 5}, {0x2b, 6},
		{0x1b, 5}
	},
/* SelIR */
	{
		{0x01, 1}, {0x01, 2}, {0x05, 3}, {0x05, 4}, {0x05, 5},
		{0x15, 5}, {0x15, 6}, {0x55, 7}, {0x35, 6}, {0x00, 0},
		{0x00, 1}, {0x00, 2}, {0x02, 2}, {0x02, 3}, {0x0a, 4},
		{0x06, 3}
	},
/* CapIR */
	{
		{0x1f, 5}, {0x03, 3}, {0x07, 3}, {0x07, 4}, {0x07, 5},
		{0x17, 5}, {0x17, 6}, {0x57, 7}, {0x37, 6}, {0x0f, 4},
		{0x00, 0}, {0x00, 1}, {0x01, 1}, {0x01, 2}, {0x05, 3},
		{0x03, 2}
	},
/* SIR */
	{
		{0x1f, 5}, {0x03, 3}, {0x07, 3}, {0x07, 4}, {0x07, 5},
		{0x17, 5}, {0x17, 6}, {0x57, 7}, {0x37, 6}, {0x0f, 4},
		{0x0f, 5}, {0x00, 0}, {0x01, 1}, {0x01, 2}, {0x05, 3},
		{0x03, 2}
	},
/* Ex1IR */
	{
		{0x0f, 4}, {0x01, 2}, {0x03, 2}, {0x03, 3}, {0x03, 4},
		{0x0b, 4}, {0x0b, 5}, {0x2b, 6}, {0x1b, 5}, {0x07, 3},
		{0x07, 4}, {0x02, 3}, {0x00, 0}, {0x00, 1}, {0x02, 2},
		{0x01, 1}
	},
/* PIR */
	{
		{0x1f, 5}, {0x03, 3}, {0x07, 3}, {0x07, 4}, {0x07, 5},
		{0x17, 5}, {0x17, 6}, {0x57, 7}, {0x37, 6}, {0x0f, 4},
		{0x0f, 5}, {0x01, 2}, {0x05, 3}, {0x00, 1}, {0x01, 1},
		{0x03, 2}
	},
/* Ex2IR */
	{
		{0x0f, 4}, {0x01, 2}, {0x03, 2}, {0x03, 3}, {0x03, 4},
		{0x0b, 4}, {0x0b, 5}, {0x2b, 6}, {0x1b, 5}, {0x07, 3},
		{0x07, 4}, {0x00, 1}, {0x02, 2}, {0x02, 3}, {0x00, 0},
		{0x01, 1}
	},
/* UpdIR */
	{
		{0x07, 3}, {0x00, 1}, {0x01, 1}, {0x01, 2}, {0x01, 3},
		{0x05, 3}, {0x05, 4}, {0x15, 5}, {0x0d, 4}, {0x03, 2},
		{0x03, 3}, {0x03, 4}, {0x0b, 4}, {0x0b, 5}, {0x2b, 6},
		{0x00, 0}
	},
};

static inline void pspi_dump_regs(struct npcm_pspi *pspi)
{
	pr_info("PSPI STAT: 0x%x\n", readb(pspi->base + PSPI_STAT));
	pr_info("PSPI CTL1: 0x%x\n", readb(pspi->base + PSPI_CTL1));
}

static inline void set_gpio(struct jtag_info *jtag,
		unsigned int pin, int value)
{
	if (pin >= pin_NUM)
		return;
	if (value)
		writel(1 << jtag->pins[pin].bit_offset,
			jtag->pins[pin].reg_base + GPnDOS);
	else
		writel(1 << jtag->pins[pin].bit_offset,
			jtag->pins[pin].reg_base + GPnDOC);
}

static inline int get_gpio(struct jtag_info *jtag,
		unsigned int pin)
{
	unsigned int value = 0;

	if (pin >= pin_NUM)
		return 0;
	if (pin == pin_TDO)
		value = readl(jtag->pins[pin].reg_base + GPnDIN);
	else
		value = readl(jtag->pins[pin].reg_base + GPnDOUT);
	return (value & (1 << jtag->pins[pin].bit_offset)) ? 1 : 0;
}

static u8 TCK_Cycle(struct jtag_info *jtag,
	unsigned char no_tdo, unsigned char TMS,
	unsigned char TDI)
{
	u32 tdo = 0;

	/* IEEE 1149.1
	 * TMS & TDI shall be sampled by the test logic on the rising edge
	 * test logic shall change TDO on the falling edge
	 */
	if (jtag->gpio_ctrl == GPIOCTRL_DIRECT) {
		set_gpio(jtag, pin_TDI, (int)TDI);
		if (jtag->tms_level != (int)TMS) {
			set_gpio(jtag, pin_TMS, (int)TMS);
			jtag->tms_level = (int)TMS;
		}
		set_gpio(jtag, pin_TCK, (int)high);
		if (!no_tdo)
			tdo = get_gpio(jtag, pin_TDO);
		set_gpio(jtag, pin_TCK, (int)low);
	} else {
		gpiod_set_value(jtag->pins[pin_TDI].gpiod, (int)TDI);
		if (jtag->tms_level != (int)TMS) {
			gpiod_set_value(jtag->pins[pin_TMS].gpiod, (int)TMS);
			jtag->tms_level = (int)TMS;
		}
		gpiod_set_value(jtag->pins[pin_TCK].gpiod, (int)high);
		if (!no_tdo)
			tdo = gpiod_get_value(jtag->pins[pin_TDO].gpiod);
		gpiod_set_value(jtag->pins[pin_TCK].gpiod, (int)low);
	}
	return tdo;
}

static int pspi_send(struct npcm_pspi *priv)
{
	u8 stat;

	int bytes = priv->mode;

	if (priv->tx_bytes < bytes) {
		dev_err(priv->dev, "short tx buf\n");
		return -EINVAL;
	}

	stat = readb(priv->base + PSPI_STAT);
	if (stat & PSPI_STAT_BSY) {
		dev_err(priv->dev, "pspi state busy\n");
		return -EBUSY;
	}

	priv->tx_bytes -= bytes;;
	if (priv->mode == BIT_MODE_8) {
		writeb(REVERSE(*priv->tx_buf), priv->base + PSPI_DATA);
		priv->tx_buf++;
	} else {
		writew(REVERSE(*priv->tx_buf) << 8
			| REVERSE(*(priv->tx_buf + 1)), priv->base + PSPI_DATA);
		priv->tx_buf += 2;
	}
	return 0;
}

static int pspi_recv(struct npcm_pspi *priv)
{
	u16 val16;
	u8 val8;
	int bytes = priv->mode;

	if (priv->rx_bytes < bytes) {
		dev_err(priv->dev, "short rx buf\n");
		return -EINVAL;
	}

	priv->rx_bytes -= bytes;
	if (priv->mode == BIT_MODE_8) {
		val8 = readb(PSPI_DATA + priv->base);
		*priv->rx_buf++ = REVERSE(val8);
	} else {
		val16 = readw(PSPI_DATA + priv->base);
		*priv->rx_buf++ = REVERSE((val16 >> 8) & 0xff);
		*priv->rx_buf++ = REVERSE(val16 & 0xff);
	}

	return 0;
}

static int pspi_xfer(struct npcm_pspi *priv,
	char *tx_buf, char *rx_buf, unsigned int xfer_bytes)
{
	u16 val;
	u8 stat;
	int bytes;
	unsigned long flags;
	int ret = 0;

	bytes = priv->mode;

	if (!tx_buf || !rx_buf || !xfer_bytes)
		return -EINVAL;

	if ((xfer_bytes % bytes) != 0) {
		dev_err(priv->dev, "invalid data len\n");
		return -EINVAL;
	}

	priv->tx_bytes = xfer_bytes;
	priv->tx_buf = tx_buf;
	priv->rx_bytes = xfer_bytes;
	priv->rx_buf = rx_buf;

	reinit_completion(&priv->xfer_done);
	/* enable EIR interrupt */
	val = readw(priv->base + PSPI_CTL1);
	val &= ~(1 << PSPI_CTL1_EIW);
	val |= (1 << PSPI_CTL1_EIR);
	writew(val, priv->base + PSPI_CTL1);

	stat = readb(priv->base + PSPI_STAT);
	if ((stat & (1 << PSPI_STAT_BSY)) == 0) {
		spin_lock_irqsave(&priv->lock, flags);
		pspi_send(priv);
		spin_unlock_irqrestore(&priv->lock, flags);
	} else {
		dev_err(priv->dev, "pspi state busy\n");
		ret = -EBUSY;
		goto disable_int;
	}

	wait_for_completion(&priv->xfer_done);
disable_int:
	val &= ~(1 << PSPI_CTL1_EIR);
	writew(val, priv->base + PSPI_CTL1);

	return ret;
}

static irqreturn_t pspi_irq_handler(int irq, void *dev_id)
{
	struct npcm_pspi *priv = dev_id;
	u8 stat;

	stat = readb(priv->base + PSPI_STAT);

	if ((stat & (1 << PSPI_STAT_RBF))) {
		if (priv->rx_bytes)
			pspi_recv(priv);
		if (priv->rx_bytes == 0)
			complete(&priv->xfer_done);
	}
	if (((stat & (1 << PSPI_STAT_BSY)) == 0)) {
		if (priv->tx_bytes)
			pspi_send(priv);
	}

	return IRQ_HANDLED;
}

static inline void npcm_jtag_bitbang(struct jtag_info *jtag,
		struct tck_bitbang *bitbang)
{
	bitbang->tdo = TCK_Cycle(jtag, 0, bitbang->tms, bitbang->tdi);
}

static inline void npcm_jtag_bitbangs(struct jtag_info *jtag,
		struct bitbang_packet *bitbangs,
		struct tck_bitbang *bitbang_data)
{
	int i;

	for (i = 0; i < bitbangs->length; i++) {
		npcm_jtag_bitbang(jtag, &bitbang_data[i]);
	}
}

static int npcm_jtag_set_tapstate(struct jtag_info *jtag,
	enum JtagStates from_state, enum JtagStates end_state)
{
	unsigned char i;
	unsigned char tmsbits;
	unsigned char count;
	enum JtagStates from, to;

	from = from_state;
	to = end_state;
	if (from == JTAG_STATE_CURRENT)
		from = jtag->tapstate;

	if ((from > JTAG_STATE_CURRENT) || (to > JTAG_STATE_CURRENT))
		return -1;

	if (to == JtagTLR) {
		for (i = 0; i < 9; i++)
			TCK_Cycle(jtag, 1, 1, 0);
		jtag->tapstate = JtagTLR;
		return 0;
	}

	tmsbits = _tmsCycleLookup[from][to].tmsbits;
	count   = _tmsCycleLookup[from][to].count;

	if (count == 0)
		return 0;

	for (i = 0; i < count; i++) {
		TCK_Cycle(jtag, 1, (tmsbits & 1), 0);
		tmsbits >>= 1;
	}
	JTAG_DBUG("jtag: change state %d -> %d\n",
		from, to);
	jtag->tapstate = to;
	return 0;
}

/* configure jtag pins(except TMS) function */
static inline void npcm_jtag_config_pins(struct jtag_info *jtag,
		int sel_pspi)
{
	int val;
	if (jtag->controller == PSPI1) {
		val = sel_pspi ? PSPI1SEL_PSPI : PSPI1SEL_GPIO;
		regmap_update_bits(jtag->gcr_regmap, MFSEL3_OFFSET,
			(PSPI1SEL_MASK << PSPI1SEL_OFFSET),
			(val << PSPI1SEL_OFFSET));
	} else if (jtag->controller == PSPI2) {
		val = sel_pspi ? PSPI2SEL_PSPI : PSPI2SEL_GPIO;
		regmap_update_bits(jtag->gcr_regmap, MFSEL3_OFFSET,
			(PSPI2SEL_MASK << PSPI2SEL_OFFSET),
			(val << PSPI2SEL_OFFSET));
	}
}

static void jtag_switch_pspi(struct jtag_info *jtag,
		bool enable)
{
	struct npcm_pspi *priv = &jtag->pspi;
	int divisor;

	if (enable) {
		divisor = (priv->apb_clk_rate / (2 * jtag->freq)) - 1;
		if (divisor <= 0) {
			dev_err(jtag->dev, "Invalid PSPI frequency\n");
			return;
		}

		/* disable */
		writew(readw(priv->base + PSPI_CTL1) & ~(0x1 << PSPI_CTL1_SPIEN),
			priv->base + PSPI_CTL1);

		/* configure pin function to pspi */
		npcm_jtag_config_pins(jtag, 1);

		/* configure Shift Clock Divider value */
		writew((readw(priv->base + PSPI_CTL1) & ~(0x7f << PSPI_CTL1_SCDV6_0)) |
				(divisor << PSPI_CTL1_SCDV6_0),
				priv->base + PSPI_CTL1);

		/* configure TCK to be low when idle */
		writew(readw(priv->base + PSPI_CTL1) &
				~(0x1 << PSPI_CTL1_SCIDL),
				priv->base + PSPI_CTL1);

		/* TDI is shifted out on the falling edge,
		 * TDO is sampled on the rising edge
		 */
		writew(readw(priv->base + PSPI_CTL1) &
				~(0x1 << PSPI_CTL1_SCM),
				priv->base + PSPI_CTL1);

		/* set 16 bit mode and enable pspi */
		writew(readw(priv->base + PSPI_CTL1) | (0x1 << PSPI_CTL1_SPIEN)
				| (1 << 2), priv->base + PSPI_CTL1);

		if (readb(priv->base + PSPI_STAT) & (0x1 << PSPI_STAT_RBF))
			readw(priv->base + PSPI_STAT);
	} else {
		writew(readw(priv->base + PSPI_CTL1) & ~(0x1 << PSPI_CTL1_SPIEN),
			priv->base + PSPI_CTL1);
		npcm_jtag_config_pins(jtag, 0);

		jtag->tms_level = gpiod_get_value(jtag->pins[pin_TMS].gpiod);
	}
}

static int npcm_jtag_readwrite_scan(struct jtag_info *jtag,
		struct scan_xfer *scan_xfer, u8 *tdi, u8 *tdo)
{
	struct npcm_pspi *pspi = &jtag->pspi;
	unsigned int unit_len = pspi->mode * 8;
	unsigned int remain_bits = scan_xfer->length;
	unsigned int bit_index = 0;
	unsigned int use_pspi = 0, use_gpio = 0;
	unsigned int xfer_bytes, xfer_bits = remain_bits;
	unsigned int tdi_bytes = scan_xfer->tdi_bytes;
	unsigned int tdo_bytes = scan_xfer->tdo_bytes;
	u8 *tdi_p = tdi;
	u8 *tdo_p = tdo;
	int ret;

	if ((jtag->tapstate != JtagShfDR) &&
		(jtag->tapstate != JtagShfIR)) {
		dev_err(jtag->dev, "bad current tapstate %d\n",
				jtag->tapstate);
		return -EINVAL;
	}
	if (scan_xfer->length == 0) {
		dev_err(jtag->dev, "bad length 0\n");
		return -EINVAL;
	}

	if (tdi == NULL && scan_xfer->tdi_bytes != 0) {
		dev_err(jtag->dev, "null tdi with nonzero length %u!\n",
			scan_xfer->tdi_bytes);
		return -EINVAL;
	}

	if (tdo == NULL && scan_xfer->tdo_bytes != 0) {
		dev_err(jtag->dev, "null tdo with nonzero length %u!\n",
			scan_xfer->tdo_bytes);
		return -EINVAL;
	}

	if ((jtag->mode == MODE_PSPI) &&
		(remain_bits > unit_len)) {
		jtag_switch_pspi(jtag, true);
		use_pspi = 1;
	}

	/* handle pspi transfer with irq enabled */
	if (use_pspi && pspi->enable_irq) {
		xfer_bytes = (remain_bits / unit_len) * (unit_len / 8);

		/* the last tranfer must be transmitted using bitbang
		   to toggle tms signal */
		if (((remain_bits % unit_len) == 0) &&
				(xfer_bytes > 0))
			xfer_bytes -= (unit_len /8);

		ret = pspi_xfer(pspi, tdi_p, tdo_p, xfer_bytes);
		if (ret) {
			dev_err(jtag->dev, "pspi_xfer err\n");
			jtag_switch_pspi(jtag, false);
			return ret;
		}
		remain_bits -= (xfer_bytes * 8);
		xfer_bits = remain_bits;
		tdi_p += xfer_bytes;
		tdo_p += xfer_bytes;
		tdi_bytes -= xfer_bytes;
		tdo_bytes -= xfer_bytes;
	}

	while (bit_index < xfer_bits) {
		unsigned long timeout;
		int bit_offset = (bit_index % 8);
		int this_input_bit = 0;
		int tms_high_or_low;
		int this_output_bit;
		u16 tdo_byte;

		/* last transfer are transmitted using gpio bitbang */
		if ((jtag->mode != MODE_PSPI) || (remain_bits < unit_len) ||
			((remain_bits == unit_len) &&
			 (scan_xfer->end_tap_state != JtagShfDR)))
			use_gpio = 1;
		else
			use_gpio = 0;

		if (use_gpio) {
			/* transmit using gpio bitbang */
			if (use_pspi) {
				jtag_switch_pspi(jtag, false);
				use_pspi = 0;
			}
			if (bit_index / 8 < tdi_bytes)
				this_input_bit = (*tdi_p >> bit_offset) & 1;

			/* If this is the last bit, leave TMS high */
			tms_high_or_low = (bit_index == xfer_bits - 1) &&
				(scan_xfer->end_tap_state != JtagShfDR) &&
				(scan_xfer->end_tap_state != JtagShfIR);
			this_output_bit = TCK_Cycle(jtag, 0, tms_high_or_low, this_input_bit);
			/* If it was the last bit in the scan and the end_tap_state is
			 * something other than shiftDR or shiftIR then go to Exit1.
			 * IMPORTANT Note: if the end_tap_state is ShiftIR/DR and the
			 * next call to this function is a shiftDR/IR then the driver
			 * will not change state!
			 */
			if (tms_high_or_low) {
				jtag->tapstate = (jtag->tapstate == JtagShfDR) ?
					JtagEx1DR : JtagEx1IR;
			}
			if (bit_index / 8 < tdo_bytes) {
				if (bit_index % 8 == 0) {
					/* Zero the output buffer before writing data */
					*tdo_p = 0;
				}
				*tdo_p |= this_output_bit << bit_offset;
			}
			/* reach byte boundary, approach to next byte */
			if (bit_offset == 7) {
				tdo_p++;
				tdi_p++;
			}
			bit_index++;
		} else {
			/* transmit using pspi */
			if (!use_pspi) {
				jtag_switch_pspi(jtag, true);
				use_pspi = 1;
			}
			/* PSPI is 16 bit transfer mode */
			timeout = jiffies + msecs_to_jiffies(100);
			while (readb(pspi->base + PSPI_STAT) &
					(0x1 << PSPI_STAT_BSY)) {
				if (time_after(jiffies, timeout)) {
					jtag_switch_pspi(jtag, false);
					dev_err(jtag->dev, "PSPI_STAT_BSY timeout\n");
					return -ETIMEDOUT;
				}
				cond_resched();
			}

			if (((bit_index / 8) + 1) < tdi_bytes)
				writew(REVERSE(*tdi_p) << 8 | REVERSE(*(tdi_p+1)),
					pspi->base + PSPI_DATA);
			else
				writew(0x0, pspi->base + PSPI_DATA);

			timeout = jiffies + msecs_to_jiffies(100);
			while (!(readb(pspi->base + PSPI_STAT) &
					(0x1 << PSPI_STAT_RBF))) {
				if (time_after(jiffies, timeout)) {
					jtag_switch_pspi(jtag, false);
					dev_err(jtag->dev, "PSPI_STAT_RBF timeout\n");
					pspi_dump_regs(&jtag->pspi);
					return -ETIMEDOUT;
				}
				cond_resched();
			}

			tdo_byte = readw(pspi->base + PSPI_DATA);
			if ((bit_index / 8) + 1 < tdo_bytes) {
				*tdo_p = REVERSE((tdo_byte >> 8) & 0xff);
				*(tdo_p + 1) = REVERSE(tdo_byte & 0xff);
			}

			bit_index += unit_len;
			remain_bits -= unit_len;
			tdo_p += unit_len / 8;
			tdi_p += unit_len / 8;
		}
	}
	npcm_jtag_set_tapstate(jtag, JTAG_STATE_CURRENT,
		scan_xfer->end_tap_state);

	return 0;
}

static int npcm_jtag_xfer(struct jtag_info *jtag,
		struct jtag_xfer *xfer, u8 *data, u32 bytes)
{
	struct scan_xfer scan;
	u8 *tdo;
	int ret;

	tdo = kmalloc(bytes, GFP_KERNEL);
	if (tdo == NULL)
		return -ENOMEM;

	if (xfer->type == JTAG_SIR_XFER)
		npcm_jtag_set_tapstate(jtag, xfer->from,
					  JtagShfIR);
	else
		npcm_jtag_set_tapstate(jtag, xfer->from,
					  JtagShfDR);
	scan.end_tap_state = xfer->endstate;
	scan.length = xfer->length;
	scan.tdi_bytes = scan.tdo_bytes = bytes;

	ret = npcm_jtag_readwrite_scan(jtag, &scan, data, tdo);
	memcpy(data, tdo, bytes);
	kfree(tdo);

	return ret;
}

/* Run in current state for specific number of tcks */
static int npcm_jtag_runtest(struct jtag_info *jtag,
		unsigned int tcks)
{
	struct npcm_pspi *pspi = &jtag->pspi;
	unsigned int unit_len = pspi->mode * 8;
	unsigned int units = tcks  / unit_len;
	unsigned int bytes = units * pspi->mode;
	unsigned int remain_bits = tcks % unit_len;
	char *txbuf, *rxbuf;
	unsigned int i, ret;
	unsigned long timeout;

	if (jtag->mode != MODE_PSPI) {
		for (i = 0; i < tcks; i++) {
			TCK_Cycle(jtag, 0, 0, 0);
			cond_resched();
		}
		return 0;
	}

	if (units == 0) {
		for (i = 0; i < remain_bits; i++)
			TCK_Cycle(jtag, 0, 0, 0);
		return 0;
	}

	jtag_switch_pspi(jtag, true);

	if (jtag->pspi.enable_irq) {
		txbuf = kzalloc(bytes, GFP_KERNEL);
		if (!txbuf) {
			dev_err(jtag->dev, "kzalloc err\n");
			ret = -ENOMEM;
			goto err_pspi;
		}
		rxbuf = kzalloc(bytes, GFP_KERNEL);
		if (!rxbuf) {
			dev_err(jtag->dev, "kzalloc err\n");
			ret = -ENOMEM;
			goto err_pspi;
		}
		ret = pspi_xfer(&jtag->pspi, txbuf, rxbuf, bytes);
		kfree(txbuf);
		kfree(rxbuf);
		units = 0;
		if (ret)
			goto err_pspi;
	}

	for (i = 0; i < units; i++) {

		timeout = jiffies + msecs_to_jiffies(100);
		while (readb(pspi->base + PSPI_STAT) &
				(0x1 << PSPI_STAT_BSY)) {
			if (time_after(jiffies, timeout)) {
				ret = -ETIMEDOUT;
				goto err_pspi;
			}
			cond_resched();
		}

		writew(0x0, pspi->base + PSPI_DATA);

		timeout = jiffies + msecs_to_jiffies(100);
		while (!(readb(pspi->base + PSPI_STAT) &
				(0x1 << PSPI_STAT_RBF))) {
			if (time_after(jiffies, timeout)) {
				ret = -ETIMEDOUT;
				goto err_pspi;
			}
			cond_resched();
		}
		readw(pspi->base + PSPI_DATA);
	}

	jtag_switch_pspi(jtag, false);

	if (remain_bits) {
		for (i = 0; i < remain_bits; i++)
			TCK_Cycle(jtag, 0, 0, 0);
	}
	return 0;

err_pspi:
	jtag_switch_pspi(jtag, false);
	return ret;
}

static long jtag_ioctl(struct file *file,
		unsigned int cmd, unsigned long arg)
{
	struct jtag_info *priv = file->private_data;
	struct jtag_tap_state tapstate;
	void __user *argp = (void __user *)arg;
	struct jtag_xfer xfer;
	struct bitbang_packet bitbang;
	struct tck_bitbang *bitbang_data;
	u8 *xfer_data;
	u32 data_size;
	u32 value;
	int ret = 0;

	switch (cmd) {
	case JTAG_SIOCFREQ:
		if (get_user(value, (__u32 __user *)arg))
			return -EFAULT;
		if (value <= JTAG_PSPI_MAX_FREQ)
			priv->freq = value;
		else {
			dev_err(priv->dev, "%s: invalid jtag freq %u\n",
				__func__, value);
			ret = -EINVAL;
		}
		break;
	case JTAG_GIOCFREQ:
		if (put_user(priv->freq, (__u32 __user *)arg))
			return -EFAULT;
		break;
	case JTAG_IOCBITBANG:
		if (copy_from_user(&bitbang, (const void __user *)arg,
		   sizeof(struct bitbang_packet)))
			return -EFAULT;

		if (bitbang.length >= JTAG_MAX_XFER_DATA_LEN)
			return -EINVAL;

		data_size = bitbang.length * sizeof(struct tck_bitbang);
		bitbang_data = memdup_user((void __user *)bitbang.data,
					   data_size);
		if (IS_ERR(bitbang_data))
			return -EFAULT;

		npcm_jtag_bitbangs(priv, &bitbang, bitbang_data);
		ret = copy_to_user((void __user *)bitbang.data,
				   (void *)bitbang_data, data_size);
		kfree(bitbang_data);
		if (ret)
			return -EFAULT;
		break;
	case JTAG_SIOCSTATE:
		if (copy_from_user(&tapstate, (const void __user *)arg,
				   sizeof(struct jtag_tap_state)))
			return -EFAULT;

		if (tapstate.from > JTAG_STATE_CURRENT)
			return -EINVAL;

		if (tapstate.endstate > JTAG_STATE_CURRENT)
			return -EINVAL;

		if (tapstate.reset > JTAG_FORCE_RESET)
			return -EINVAL;
		if (tapstate.reset == JTAG_FORCE_RESET)
			npcm_jtag_set_tapstate(priv, JTAG_STATE_CURRENT, JtagTLR);
		npcm_jtag_set_tapstate(priv, tapstate.from, tapstate.endstate);
		break;
	case JTAG_GIOCSTATUS:
		ret = put_user(priv->tapstate, (__u32 __user *)arg);
		break;
	case JTAG_IOCXFER:
		if (copy_from_user(&xfer, argp, sizeof(struct jtag_xfer)))
			return -EFAULT;

		if (xfer.length >= JTAG_MAX_XFER_DATA_LEN)
			return -EINVAL;

		if (xfer.type > JTAG_SDR_XFER)
			return -EINVAL;

		if (xfer.direction > JTAG_READ_WRITE_XFER)
			return -EINVAL;

		if (xfer.from > JTAG_STATE_CURRENT)
			return -EINVAL;

		if (xfer.endstate > JTAG_STATE_CURRENT)
			return -EINVAL;

		data_size = DIV_ROUND_UP(xfer.length, BITS_PER_BYTE);
		xfer_data = memdup_user(u64_to_user_ptr(xfer.tdio), data_size);
		if (IS_ERR(xfer_data))
			return -EFAULT;
		ret = npcm_jtag_xfer(priv, &xfer, xfer_data, data_size);
		if (ret) {
			kfree(xfer_data);
			return -EIO;
		}
		ret = copy_to_user(u64_to_user_ptr(xfer.tdio),
				   (void *)xfer_data, data_size);
		kfree(xfer_data);
		if (ret)
			return -EFAULT;

		if (copy_to_user((void __user *)arg, (void *)&xfer,
				 sizeof(struct jtag_xfer)))
			return -EFAULT;
		break;
	case JTAG_SIOCMODE:
		break;
	case JTAG_RUNTEST:
		ret = npcm_jtag_runtest(priv, (unsigned int)arg);
		break;
	case JTAG_DIRECTGPIO:
		if (!arg)
			priv->gpio_ctrl = GPIOCTRL_API;
		else
			priv->gpio_ctrl = GPIOCTRL_DIRECT;
		break;
	case JTAG_PSPI:
		if (!arg)
			priv->mode = MODE_GPIO;
		else
			priv->mode = MODE_PSPI;
		break;
	case JTAG_PSPI_IRQ:
		if (!arg)
			priv->pspi.enable_irq = false;
		else
			priv->pspi.enable_irq = true;
		break;
	case JTAG_SLAVECONTLR:
		break;
	default:
		return -ENOTTY;
	}

	return ret;
}

static int jtag_open(struct inode *inode, struct file *file)
{
	struct jtag_info *jtag;
	jtag = container_of(file->private_data, struct jtag_info, miscdev);

	spin_lock(&jtag_file_lock);
	if (jtag->is_open) {
		spin_unlock(&jtag_file_lock);
		return -EBUSY;
	}

	jtag->is_open = true;
	file->private_data = jtag;

	spin_unlock(&jtag_file_lock);

	return 0;
}

static int jtag_release(struct inode *inode, struct file *file)
{
	struct jtag_info *jtag = file->private_data;
	spin_lock(&jtag_file_lock);
	jtag->is_open = false;
	spin_unlock(&jtag_file_lock);

	return 0;
}

const struct file_operations npcm_jtag_fops = {
	.open              = jtag_open,
	.unlocked_ioctl    = jtag_ioctl,
	.release           = jtag_release,
};


static int jtag_register_device(struct jtag_info *jtag)
{
	struct device *dev = jtag->dev;
	static int dev_num = 0;
	int err;

	if (!dev)
		return -ENODEV;

	/* register miscdev */
	jtag->miscdev.parent = dev;
	jtag->miscdev.fops =  &npcm_jtag_fops;
	jtag->miscdev.minor = MISC_DYNAMIC_MINOR;
	jtag->miscdev.name = kasprintf(GFP_KERNEL, "jtag%d", dev_num++);
	if (!jtag->miscdev.name)
		return -ENOMEM;

	err = misc_register(&jtag->miscdev);
	if (err) {
		dev_err(jtag->miscdev.parent,
			"Unable to register device, err %d\n", err);
		kfree(jtag->miscdev.name);
		return err;
	}

	return 0;
}

static void npcm_jtag_init(struct jtag_info *priv)
{
	priv->freq = JTAG_PSPI_SPEED;
	priv->pspi.mode = BIT_MODE_16;
	priv->pspi.enable_irq = false;

	/* initialize pins to gpio function */
	npcm_jtag_config_pins(priv, 0);
	gpiod_direction_output(priv->pins[pin_TCK].gpiod, 0);
	gpiod_direction_output(priv->pins[pin_TDI].gpiod, 1);
	gpiod_direction_input(priv->pins[pin_TDO].gpiod);
	gpiod_direction_output(priv->pins[pin_TMS].gpiod, 1);
	priv->tms_level = gpiod_get_value(priv->pins[pin_TMS].gpiod);

	npcm_jtag_set_tapstate(priv, JTAG_STATE_CURRENT, JtagTLR);
}

static int npcm_jtag_pspi_probe(struct platform_device *pdev,
		struct npcm_pspi *priv)
{
	struct resource *res;
	struct clk *apb_clk;
	int irq;
	int ret = 0;
	dev_info(&pdev->dev, "npcm_jtag_pspi_probe\n");

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	priv->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(priv->base)) {
		return PTR_ERR(priv->base);
	}
	priv->dev = &pdev->dev;

	apb_clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(apb_clk)) {
		dev_err(&pdev->dev, "can't read apb clk\n");
		return -ENODEV;
	}
	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(&pdev->dev, "failed to get IRQ\n");
		return irq;
	}

	ret = devm_request_irq(&pdev->dev, irq, pspi_irq_handler, 0,
			       "npcm-jtag-master", priv);
	if (ret) {
		dev_err(&pdev->dev, "failed to request IRQ\n");
		return ret;
	}

	spin_lock_init(&priv->lock);
	init_completion(&priv->xfer_done);

	clk_prepare_enable(apb_clk);

	priv->apb_clk_rate = clk_get_rate(apb_clk);

	return 0;
}

static int npcm_jtag_probe(struct platform_device *pdev)
{
	struct jtag_info *npcm_jtag;
	struct gpio_desc *gpiod;
	struct gpio_chip *chip;
	void __iomem *gpio_base;
	u32 value;
	u32 gpios_reg[pin_NUM];
	int i, ret;
	enum gpiod_flags pin_flags[pin_NUM] = {
		GPIOD_OUT_LOW, GPIOD_OUT_HIGH,
		GPIOD_IN, GPIOD_OUT_HIGH,
		};

	dev_info(&pdev->dev, "npcm_jtag_probe\n");

	npcm_jtag = kzalloc(sizeof(struct jtag_info), GFP_KERNEL);
	if (!npcm_jtag)
		return -ENOMEM;
	npcm_jtag->dev = &pdev->dev;

	npcm_jtag->gcr_regmap =
		syscon_regmap_lookup_by_compatible("nuvoton,npcm750-gcr");
	if (IS_ERR(npcm_jtag->gcr_regmap)) {
		dev_err(&pdev->dev, "can't find npcm750-gcr\n");
		ret = PTR_ERR(npcm_jtag->gcr_regmap);
		goto err;
	}

	npcm_jtag->gpio_ctrl = GPIOCTRL_DIRECT;
	npcm_jtag->mode = MODE_PSPI;

	/* jtag gpios ctrl register*/
	ret = of_property_read_u32_array(pdev->dev.of_node,
				   "jtag-gpios-reg", &gpios_reg[0], pin_NUM);
	if (ret < 0) {
		dev_info(&pdev->dev, "No GPIO regs");
		return -EINVAL;
	}

	/* jtag pins */
	for (i = 0; i < pin_NUM; i++) {
		gpiod = gpiod_get_index(&pdev->dev, "jtag",
			i, pin_flags[i]);
		if (IS_ERR(gpiod)) {
			dev_err(&pdev->dev, "No jtag pin: %d", i);
			return PTR_ERR(gpiod);
		}
		chip = gpiod_to_chip(gpiod);
		npcm_jtag->pins[i].gpiod = gpiod;

		npcm_jtag->pins[i].bit_offset = desc_to_gpio(gpiod)
			- chip->base;
		gpio_base = ioremap(gpios_reg[i], 0x80);
		npcm_jtag->pins[i].reg_base = gpio_base;
		if (IS_ERR(gpio_base)) {
			dev_err(&pdev->dev, "unable to map iobase");
			return PTR_ERR(gpio_base);
		}
	}

	/* setup pspi */
	value = PSPI1;
	ret = of_property_read_u32(pdev->dev.of_node,
			"pspi-controller", &value);
	if (ret < 0 || (value != PSPI1 && value != PSPI2))
		dev_err(&pdev->dev,
				"Could not read pspi index\n");
	npcm_jtag->controller = value;
	npcm_jtag_pspi_probe(pdev, &npcm_jtag->pspi);

	npcm_jtag_init(npcm_jtag);

	ret = jtag_register_device(npcm_jtag);
	if (ret) {
		dev_err(&pdev->dev, "failed to create device\n");
		goto err;
	}
	platform_set_drvdata(pdev, npcm_jtag);

	return 0;
err:
	kfree(npcm_jtag);
	return ret;
}

static int npcm_jtag_remove(struct platform_device *pdev)
{
	struct jtag_info *jtag = platform_get_drvdata(pdev);
	int i;

	if (!jtag)
		return 0;

	misc_deregister(&jtag->miscdev);
	kfree(jtag->miscdev.name);
	for (i = 0; i < pin_NUM; i++) {
		iounmap(jtag->pins[i].reg_base);
		gpiod_put(jtag->pins[i].gpiod);
	}
	kfree(jtag);

	return 0;
}


static const struct of_device_id npcm_jtag_id[] = {
	{ .compatible = "nuvoton,npcm750-jtag-master", },
	{},
};
MODULE_DEVICE_TABLE(of, npcm_jtag_id);

static struct platform_driver npcm_jtag_driver = {
	.probe          = npcm_jtag_probe,
	.remove			= npcm_jtag_remove,
	.driver         = {
		.name   = "jtag-master",
		.owner	= THIS_MODULE,
		.of_match_table = npcm_jtag_id,
	},
};

module_platform_driver(npcm_jtag_driver);

MODULE_AUTHOR("Nuvoton Technology Corp.");
MODULE_DESCRIPTION("JTAG Master Driver");
MODULE_LICENSE("GPL");

