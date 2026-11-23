// SPDX-License-Identifier: GPL-2.0-only
/*
 * Realtek RTL9607DQ SSP (PCM/TDM) controller
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 *
 * The SSP is the voice PCM interface of the SoC. It clocks a TDM frame of up
 * to 32 slots at 8 kHz and moves the valid slots through one DMA queue pair
 * of the DMA_SEC block. Each queue is a ring of 8-byte descriptors; the
 * hardware advances the RX write index and the TX read index, the driver the
 * other two; the indices wrap at the ring depth and a queue runs until its
 * read index meets its write index. One descriptor covers one ALSA period, so
 * the ring depth is the period count and must be a power of two.
 *
 * The queue interrupts do not reach the CPU on this SoC, so an hrtimer polls
 * the queue indices twice per period, refills the playback queue and returns
 * the capture descriptors to the hardware. The frame clock stops as soon as
 * the playback queue runs dry.
 *
 * The capture DMA only completes descriptors of whole 128-byte bursts, which
 * a 10 ms period of one 16-bit slot (160 bytes) is not. Capture therefore
 * runs a fixed ring of 128-byte chunks independent of the ALSA period, and
 * every descriptor handed back is pointed at the next chunk of the buffer.
 *
 * The DMA_SEC window is shared with the network engine, which maps all of
 * it, so the regions of this block are mapped without being requested.
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/hrtimer.h>
#include <linux/io.h>
#include <linux/log2.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#define DRV_NAME			"rtl9607dq-ssp"

/* SSP block */
#define SSP_CTRL0			0x04
#define  SSP_CTRL0_RX_BYTE_SWAP		BIT(0)
#define  SSP_CTRL0_TX_BYTE_SWAP		BIT(1)
#define  SSP_CTRL0_LOOP_EN		BIT(2)
#define  SSP_CTRL0_ZSI_SEL_EN		BIT(14)
#define  SSP_CTRL0_ISI_EN		BIT(15)
#define  SSP_CTRL0_SPI_SEL		GENMASK(27, 25)
#define SSP_CTRL1			0x08
#define  SSP_CTRL1_START		BIT(0)
#define SSP_FRAME_CTRL0			0x0c
#define  SSP_FRAME_CTRL0_EXT_CLK_SEL	BIT(17)
#define  SSP_FRAME_CTRL0_PLL_49M_EN	BIT(25)
#define  SSP_FRAME_CTRL0_RX_EDGE	BIT(26)
#define SSP_FRAME_CTRL1			0x10
#define  SSP_FRAME_CTRL1_FS_LEN		GENMASK(9, 0)
#define  SSP_FRAME_CTRL1_SLOT2FS	GENMASK(15, 12)
#define  SSP_FRAME_CTRL1_SLOTS		GENMASK(22, 16)
#define  SSP_FRAME_CTRL1_SLOT2SLOT	GENMASK(26, 24)
#define  SSP_FRAME_CTRL1_FS2SLOT	GENMASK(31, 28)
#define SSP_BAUDRATE_CTRL		0x14
#define SSP_FIFO_CTRL			0x18
#define SSP_INTERRUPT			0x20
#define SSP_TXSLOT_VLD(n)		(0x28 + (n) * 4)
#define SSP_RXSLOT_VLD(n)		(0x38 + (n) * 4)
#define SSP_SLOT_SIZE(n)		(0x48 + (n) * 4)
#define SSP_BAUDRATE_CTRL1		0x58
#define SSP_SPARE			0x64
#define  SSP_SPARE_RESET		BIT(0)

/*
 * Bit clock settings for a 2.048 MHz PCLK (32 slots of 8 bits at 8 kHz)
 * from the 49.152 MHz audio PLL, as the vendor firmware programs them.
 */
#define SSP_BAUDRATE_2048K		0x15555555
#define SSP_BAUDRATE1_2048K		0xfffffffc
#define SSP_FIFO_CTRL_DEFAULT		0x07f4007f

/* DMA_SEC global SSP DMA control */
#define DMA_RX_CTRL			0x00
#define DMA_TX_CTRL			0x04
#define  DMA_CTRL_ENABLE		BIT(0)
#define  DMA_TX_CTRL_SRESET		BIT(3)	/* clears the queue indices */

/* DMA_SEC SSP queue pair */
#define Q_RXQ_BASE_DEPTH		0x04
#define Q_RXQ_BASE			0x08
#define Q_RXQ_WPTR			0x0c
#define Q_RXQ_RPTR			0x10
#define Q_TXQ_CONTROL			0x18
#define  Q_TXQ_CONTROL_EN		BIT(0)
#define Q_TXQ_BASE_DEPTH		0x1c
#define Q_TXQ_BASE			0x20
#define Q_TXQ_WPTR			0x24
#define Q_TXQ_RPTR			0x28
#define Q_PTR_INDEX			GENMASK(12, 0)

#define SSP_DESC_LEN			GENMASK(23, 8)
#define SSP_DESC_TX			BIT(24)

#define SSP_MAX_SLOTS			32
#define SSP_SLOT_WIDTH			8
#define SSP_MAX_PERIODS			64
#define SSP_MAX_PERIOD_BYTES		640
#define SSP_RX_CHUNK_BYTES		128
#define SSP_RX_RING			16

struct rtl9607dq_ssp_desc {
	__le32 addr;
	__le32 ctrl;
};

struct rtl9607dq_ssp {
	struct device *dev;
	void __iomem *base;
	void __iomem *dma;
	void __iomem *queue;
	struct hrtimer timer;
	ktime_t poll_time;
	ktime_t poll[2];

	struct rtl9607dq_ssp_desc *desc[2];
	dma_addr_t desc_dma[2];
	struct snd_pcm_substream *substream[2];
	unsigned int periods[2];
	unsigned int last[2];

	dma_addr_t rx_dma;
	unsigned int rx_chunks;
	unsigned int rx_seq;

	u32 tx_mask;
	u32 rx_mask;
	unsigned int slots;
	unsigned int fs2slot;
	unsigned int running;
	/* protects the queue state shared by trigger and the poll timer */
	spinlock_t lock;
};

/*
 * The queues wedge on rings of fewer than eight descriptors and on
 * descriptors longer than 640 bytes (10 ms of the stock 8-byte frame).
 */
static const unsigned int rtl9607dq_ssp_periods[] = { 8, 16, 32, 64 };

static const struct snd_pcm_hw_constraint_list rtl9607dq_ssp_periods_list = {
	.count = ARRAY_SIZE(rtl9607dq_ssp_periods),
	.list = rtl9607dq_ssp_periods,
};

static const struct snd_pcm_hardware rtl9607dq_ssp_pcm_hardware = {
	.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_BATCH,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_8000,
	.rate_min = 8000,
	.rate_max = 8000,
	.channels_min = 1,
	.channels_max = 2,
	.buffer_bytes_max = SSP_MAX_PERIODS * SSP_MAX_PERIOD_BYTES,
	.period_bytes_min = 32,
	.period_bytes_max = SSP_MAX_PERIOD_BYTES,
	.periods_min = 8,
	.periods_max = SSP_MAX_PERIODS,
};

static inline u32 ssp_ptr(void __iomem *reg)
{
	return FIELD_GET(Q_PTR_INDEX, readl(reg));
}

static void rtl9607dq_ssp_set_frame(struct rtl9607dq_ssp *ssp)
{
	unsigned int i;

	writel(SSP_FRAME_CTRL0_EXT_CLK_SEL | SSP_FRAME_CTRL0_PLL_49M_EN |
	       SSP_FRAME_CTRL0_RX_EDGE, ssp->base + SSP_FRAME_CTRL0);
	writel(FIELD_PREP(SSP_FRAME_CTRL1_FS_LEN, 7) |
	       FIELD_PREP(SSP_FRAME_CTRL1_SLOT2FS, 7) |
	       FIELD_PREP(SSP_FRAME_CTRL1_SLOTS, ssp->slots - 1) |
	       FIELD_PREP(SSP_FRAME_CTRL1_FS2SLOT, ssp->fs2slot),
	       ssp->base + SSP_FRAME_CTRL1);
	writel(SSP_BAUDRATE_2048K, ssp->base + SSP_BAUDRATE_CTRL);
	writel(SSP_BAUDRATE1_2048K, ssp->base + SSP_BAUDRATE_CTRL1);
	writel(SSP_FIFO_CTRL_DEFAULT, ssp->base + SSP_FIFO_CTRL);

	/* every slot is 8 bits wide: four bits of (width - 1) per slot */
	for (i = 0; i < 4; i++)
		writel(0x77777777, ssp->base + SSP_SLOT_SIZE(i));

	writel(ssp->tx_mask, ssp->base + SSP_TXSLOT_VLD(0));
	writel(ssp->rx_mask, ssp->base + SSP_RXSLOT_VLD(0));
	for (i = 1; i < 4; i++) {
		writel(0, ssp->base + SSP_TXSLOT_VLD(i));
		writel(0, ssp->base + SSP_RXSLOT_VLD(i));
	}
}

static int rtl9607dq_ssp_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct rtl9607dq_ssp *ssp = snd_soc_dai_get_drvdata(dai);

	if ((fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) != SND_SOC_DAIFMT_BP_FP)
		return -EINVAL;
	if ((fmt & SND_SOC_DAIFMT_INV_MASK) != SND_SOC_DAIFMT_NB_NF)
		return -EINVAL;

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_DSP_A:
		ssp->fs2slot = 1;
		break;
	case SND_SOC_DAIFMT_DSP_B:
		ssp->fs2slot = 0;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int rtl9607dq_ssp_set_tdm_slot(struct snd_soc_dai *dai,
				      unsigned int tx_mask, unsigned int rx_mask,
				      int slots, int slot_width)
{
	struct rtl9607dq_ssp *ssp = snd_soc_dai_get_drvdata(dai);

	if (slots < 2 || slots > SSP_MAX_SLOTS || slot_width != SSP_SLOT_WIDTH)
		return -EINVAL;

	ssp->slots = slots;
	ssp->tx_mask = tx_mask;
	ssp->rx_mask = rx_mask;

	return 0;
}

static int rtl9607dq_ssp_hw_params(struct snd_pcm_substream *substream,
				   struct snd_pcm_hw_params *params,
				   struct snd_soc_dai *dai)
{
	struct rtl9607dq_ssp *ssp = snd_soc_dai_get_drvdata(dai);
	u32 mask = substream->stream == SNDRV_PCM_STREAM_PLAYBACK ?
		   ssp->tx_mask : ssp->rx_mask;

	/* the valid slots carry the frame: 8 bits of sample per slot */
	if (hweight32(mask) * SSP_SLOT_WIDTH !=
	    params_channels(params) * params_physical_width(params)) {
		dev_err(ssp->dev, "%u channels do not fit the slot mask %#x\n",
			params_channels(params), mask);
		return -EINVAL;
	}

	return 0;
}

static const struct snd_soc_dai_ops rtl9607dq_ssp_dai_ops = {
	.set_fmt = rtl9607dq_ssp_set_fmt,
	.set_tdm_slot = rtl9607dq_ssp_set_tdm_slot,
	.hw_params = rtl9607dq_ssp_hw_params,
};

static struct snd_soc_dai_driver rtl9607dq_ssp_dai = {
	.name = DRV_NAME,
	.playback = {
		.stream_name = "Playback",
		.channels_min = 1,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_8000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
	.capture = {
		.stream_name = "Capture",
		.channels_min = 1,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_8000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
	.ops = &rtl9607dq_ssp_dai_ops,
	.symmetric_rate = 1,
};

/*
 * The SLIC behind the peripheral SPI controller only answers while the SSP
 * runs in ISI mode on SPI select 1 with its bit clock programmed, and only
 * after the block has been reset following that selection; until then every
 * SPI read returns zero. Without zsi_sel_en the frame also runs at half the
 * programmed rate.
 */
static void rtl9607dq_ssp_setup(struct rtl9607dq_ssp *ssp)
{
	u32 val;

	rtl9607dq_ssp_set_frame(ssp);

	val = readl(ssp->base + SSP_CTRL0);
	val &= ~SSP_CTRL0_SPI_SEL;
	val |= FIELD_PREP(SSP_CTRL0_SPI_SEL, 1) | SSP_CTRL0_ISI_EN |
	       SSP_CTRL0_ZSI_SEL_EN | SSP_CTRL0_RX_BYTE_SWAP |
	       SSP_CTRL0_TX_BYTE_SWAP;
	writel(val, ssp->base + SSP_CTRL0);

	val = readl(ssp->base + SSP_SPARE);
	writel(val | SSP_SPARE_RESET, ssp->base + SSP_SPARE);
	writel(val & ~SSP_SPARE_RESET, ssp->base + SSP_SPARE);
}

static void rtl9607dq_ssp_start(struct rtl9607dq_ssp *ssp)
{
	/* the DMA soft reset on the first start clears the ISI selection */
	rtl9607dq_ssp_setup(ssp);
	writel(SSP_CTRL1_START, ssp->base + SSP_CTRL1);
}

static void rtl9607dq_ssp_stop(struct rtl9607dq_ssp *ssp)
{
	writel(0, ssp->base + SSP_CTRL1);
}

/*
 * Point the descriptor that fills chunk number seq at its place in the
 * buffer. The capture index does not always wrap at the ring depth, so every
 * slot that aliases it in the table gets the same address.
 */
static void rtl9607dq_ssp_rx_desc_set(struct rtl9607dq_ssp *ssp,
				      unsigned int seq)
{
	struct rtl9607dq_ssp_desc *desc = ssp->desc[SNDRV_PCM_STREAM_CAPTURE];
	u32 addr = ssp->rx_dma + (seq % ssp->rx_chunks) * SSP_RX_CHUNK_BYTES;
	unsigned int i;

	for (i = seq % SSP_RX_RING; i < SSP_MAX_PERIODS; i += SSP_RX_RING)
		desc[i].addr = cpu_to_le32(addr);
}

static void rtl9607dq_ssp_queue_start(struct rtl9607dq_ssp *ssp, int stream)
{
	unsigned int periods = ssp->periods[stream];
	u32 base_depth = lower_32_bits(ssp->desc_dma[stream]) | ilog2(periods);

	if (stream == SNDRV_PCM_STREAM_PLAYBACK) {
		writel(upper_32_bits(ssp->desc_dma[stream]), ssp->queue + Q_TXQ_BASE);
		writel(base_depth, ssp->queue + Q_TXQ_BASE_DEPTH);
		/* the whole buffer is filled before the start */
		writel(periods - 1, ssp->queue + Q_TXQ_WPTR);
		writel(Q_TXQ_CONTROL_EN, ssp->queue + Q_TXQ_CONTROL);
		writel(DMA_CTRL_ENABLE, ssp->dma + DMA_TX_CTRL);
	} else {
		for (ssp->rx_seq = 0; ssp->rx_seq < SSP_RX_RING; ssp->rx_seq++)
			rtl9607dq_ssp_rx_desc_set(ssp, ssp->rx_seq);
		ssp->rx_seq = 0;

		writel(upper_32_bits(ssp->desc_dma[stream]), ssp->queue + Q_RXQ_BASE);
		writel(base_depth, ssp->queue + Q_RXQ_BASE_DEPTH);
		writel(0, ssp->queue + Q_RXQ_RPTR);
		writel(DMA_CTRL_ENABLE, ssp->dma + DMA_RX_CTRL);
	}

	ssp->last[stream] = 0;
}

static void rtl9607dq_ssp_queue_stop(struct rtl9607dq_ssp *ssp, int stream)
{
	/* the DMA must not touch the buffer once ALSA may free it */
	if (stream == SNDRV_PCM_STREAM_PLAYBACK) {
		writel(0, ssp->queue + Q_TXQ_CONTROL);
		writel(0, ssp->dma + DMA_TX_CTRL);
	} else {
		writel(0, ssp->dma + DMA_RX_CTRL);
	}
}

/*
 * After a cold boot the ISI path only carries SPI data once the transmitter
 * has run; send a few milliseconds of silence so that the SLIC can be
 * reached before the first PCM stream.
 */
static int rtl9607dq_ssp_prime(struct rtl9607dq_ssp *ssp)
{
	const int stream = SNDRV_PCM_STREAM_PLAYBACK;
	const unsigned int periods = 8, period_bytes = 80;
	struct rtl9607dq_ssp_desc *desc = ssp->desc[stream];
	dma_addr_t dma;
	unsigned int i;
	void *buf;

	buf = dma_alloc_coherent(ssp->dev, periods * period_bytes, &dma,
				 GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	for (i = 0; i < SSP_MAX_PERIODS; i++) {
		desc[i].addr = cpu_to_le32(dma + (i % periods) * period_bytes);
		desc[i].ctrl = cpu_to_le32(FIELD_PREP(SSP_DESC_LEN, period_bytes) |
					   SSP_DESC_TX);
	}
	ssp->periods[stream] = periods;

	writel(DMA_TX_CTRL_SRESET, ssp->dma + DMA_TX_CTRL);
	writel(0, ssp->dma + DMA_TX_CTRL);
	rtl9607dq_ssp_queue_start(ssp, stream);
	rtl9607dq_ssp_start(ssp);
	msleep(20);
	rtl9607dq_ssp_stop(ssp);
	rtl9607dq_ssp_queue_stop(ssp, stream);

	dma_free_coherent(ssp->dev, periods * period_bytes, buf, dma);

	return 0;
}

/* Poll at the rate of the most demanding running stream; caller holds lock */
static void rtl9607dq_ssp_update_poll(struct rtl9607dq_ssp *ssp)
{
	ktime_t poll = KTIME_MAX;
	int i;

	for (i = 0; i < ARRAY_SIZE(ssp->substream); i++)
		if (ssp->substream[i])
			poll = min(poll, ssp->poll[i]);

	if (poll != KTIME_MAX)
		ssp->poll_time = poll;
}

static enum hrtimer_restart rtl9607dq_ssp_poll(struct hrtimer *timer)
{
	struct rtl9607dq_ssp *ssp = container_of(timer, struct rtl9607dq_ssp, timer);
	struct snd_pcm_substream *tx = NULL, *rx = NULL;
	unsigned int periods;
	unsigned long flags;
	ktime_t poll;
	u32 ptr;

	spin_lock_irqsave(&ssp->lock, flags);

	if (!ssp->running) {
		spin_unlock_irqrestore(&ssp->lock, flags);
		return HRTIMER_NORESTART;
	}

	if (ssp->substream[SNDRV_PCM_STREAM_PLAYBACK]) {
		periods = ssp->periods[SNDRV_PCM_STREAM_PLAYBACK];
		ptr = ssp_ptr(ssp->queue + Q_TXQ_RPTR) % periods;
		if (ptr != ssp->last[SNDRV_PCM_STREAM_PLAYBACK]) {
			ssp->last[SNDRV_PCM_STREAM_PLAYBACK] = ptr;
			/* keep every period but the one in flight queued */
			writel((ptr + periods - 1) % periods,
			       ssp->queue + Q_TXQ_WPTR);
			tx = ssp->substream[SNDRV_PCM_STREAM_PLAYBACK];
		}
	}

	if (ssp->substream[SNDRV_PCM_STREAM_CAPTURE]) {
		unsigned int done;

		ptr = ssp_ptr(ssp->queue + Q_RXQ_WPTR);
		done = (ptr - ssp->last[SNDRV_PCM_STREAM_CAPTURE]) &
		       (SSP_RX_RING - 1);
		if (done) {
			ssp->last[SNDRV_PCM_STREAM_CAPTURE] = ptr;
			/* hand the filled chunks back, each aimed one ring on */
			while (done--)
				rtl9607dq_ssp_rx_desc_set(ssp, ++ssp->rx_seq +
							  SSP_RX_RING - 1);
			writel(ptr, ssp->queue + Q_RXQ_RPTR);
			rx = ssp->substream[SNDRV_PCM_STREAM_CAPTURE];
		}
	}

	poll = ssp->poll_time;
	spin_unlock_irqrestore(&ssp->lock, flags);

	if (tx)
		snd_pcm_period_elapsed(tx);
	if (rx)
		snd_pcm_period_elapsed(rx);

	hrtimer_forward_now(timer, poll);

	return HRTIMER_RESTART;
}

static int rtl9607dq_ssp_pcm_open(struct snd_soc_component *component,
				  struct snd_pcm_substream *substream)
{
	int ret;

	snd_soc_set_runtime_hwparams(substream, &rtl9607dq_ssp_pcm_hardware);

	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		ret = snd_pcm_hw_constraint_step(substream->runtime, 0,
						 SNDRV_PCM_HW_PARAM_BUFFER_BYTES,
						 SSP_RX_CHUNK_BYTES);
	else
		ret = snd_pcm_hw_constraint_list(substream->runtime, 0,
						 SNDRV_PCM_HW_PARAM_PERIODS,
						 &rtl9607dq_ssp_periods_list);
	if (ret)
		return ret;

	return snd_pcm_hw_constraint_step(substream->runtime, 0,
					  SNDRV_PCM_HW_PARAM_PERIOD_BYTES, 16);
}

static int rtl9607dq_ssp_pcm_hw_params(struct snd_soc_component *component,
				       struct snd_pcm_substream *substream,
				       struct snd_pcm_hw_params *params)
{
	struct rtl9607dq_ssp *ssp = snd_soc_component_get_drvdata(component);
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct rtl9607dq_ssp_desc *desc = ssp->desc[substream->stream];
	unsigned int period_bytes = params_period_bytes(params);
	unsigned int periods = params_periods(params);
	unsigned int i, frames;

	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		ssp->rx_dma = runtime->dma_addr;
		ssp->rx_chunks = params_buffer_bytes(params) / SSP_RX_CHUNK_BYTES;
		ssp->periods[substream->stream] = SSP_RX_RING;
		for (i = 0; i < SSP_MAX_PERIODS; i++)
			desc[i].ctrl = cpu_to_le32(FIELD_PREP(SSP_DESC_LEN,
							      SSP_RX_CHUNK_BYTES));
		/* runtime->frame_bits is only set after hw_params returns */
		frames = SSP_RX_CHUNK_BYTES * 8 /
			 (params_physical_width(params) * params_channels(params));
	} else {
		for (i = 0; i < SSP_MAX_PERIODS; i++) {
			desc[i].addr = cpu_to_le32(runtime->dma_addr +
						   (i % periods) * period_bytes);
			desc[i].ctrl = cpu_to_le32(FIELD_PREP(SSP_DESC_LEN,
							      period_bytes) |
						   SSP_DESC_TX);
		}
		ssp->periods[substream->stream] = periods;
		frames = params_period_size(params);
	}

	if (!frames)
		return -EINVAL;

	/* poll twice per period, or per chunk */
	ssp->poll[substream->stream] =
		ns_to_ktime(div_u64((u64)frames * NSEC_PER_SEC,
				    params_rate(params) * 2));

	return 0;
}

static int rtl9607dq_ssp_pcm_trigger(struct snd_soc_component *component,
				     struct snd_pcm_substream *substream,
				     int cmd)
{
	struct rtl9607dq_ssp *ssp = snd_soc_component_get_drvdata(component);
	int stream = substream->stream;
	unsigned long flags;

	spin_lock_irqsave(&ssp->lock, flags);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		if (ssp->substream[stream])
			break;
		ssp->substream[stream] = substream;
		if (!ssp->running) {
			/* restart both rings at index 0 */
			writel(DMA_TX_CTRL_SRESET, ssp->dma + DMA_TX_CTRL);
			writel(0, ssp->dma + DMA_TX_CTRL);
		}
		rtl9607dq_ssp_queue_start(ssp, stream);
		rtl9607dq_ssp_update_poll(ssp);
		if (!ssp->running++) {
			rtl9607dq_ssp_start(ssp);
			hrtimer_start(&ssp->timer, ssp->poll_time,
				      HRTIMER_MODE_REL_SOFT);
		}
		break;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		if (!ssp->substream[stream])
			break;
		rtl9607dq_ssp_queue_stop(ssp, stream);
		ssp->substream[stream] = NULL;
		rtl9607dq_ssp_update_poll(ssp);
		if (!--ssp->running) {
			rtl9607dq_ssp_stop(ssp);
			hrtimer_try_to_cancel(&ssp->timer);
		}
		break;

	default:
		spin_unlock_irqrestore(&ssp->lock, flags);
		return -EINVAL;
	}

	spin_unlock_irqrestore(&ssp->lock, flags);

	return 0;
}

static snd_pcm_uframes_t
rtl9607dq_ssp_pcm_pointer(struct snd_soc_component *component,
			  struct snd_pcm_substream *substream)
{
	struct rtl9607dq_ssp *ssp = snd_soc_component_get_drvdata(component);
	struct snd_pcm_runtime *runtime = substream->runtime;
	u32 index;

	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		index = READ_ONCE(ssp->rx_seq) % ssp->rx_chunks;
		return index * bytes_to_frames(runtime, SSP_RX_CHUNK_BYTES);
	}

	index = ssp_ptr(ssp->queue + Q_TXQ_RPTR);

	return (index % runtime->periods) * runtime->period_size;
}

static int rtl9607dq_ssp_pcm_new(struct snd_soc_component *component,
				 struct snd_soc_pcm_runtime *rtd)
{
	size_t size = rtl9607dq_ssp_pcm_hardware.buffer_bytes_max;

	snd_pcm_set_managed_buffer_all(rtd->pcm, SNDRV_DMA_TYPE_DEV,
				       component->dev, size, size);

	return 0;
}

static const struct snd_soc_component_driver rtl9607dq_ssp_component = {
	.name = DRV_NAME,
	.open = rtl9607dq_ssp_pcm_open,
	.hw_params = rtl9607dq_ssp_pcm_hw_params,
	.trigger = rtl9607dq_ssp_pcm_trigger,
	.pointer = rtl9607dq_ssp_pcm_pointer,
	.pcm_construct = rtl9607dq_ssp_pcm_new,
	.legacy_dai_naming = 1,
};

static void __iomem *rtl9607dq_ssp_map(struct platform_device *pdev,
				       const char *name)
{
	struct resource *res;
	void __iomem *base;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	if (!res)
		return IOMEM_ERR_PTR(-EINVAL);

	/* shared with the network engine, which requests the whole window */
	base = devm_ioremap(&pdev->dev, res->start, resource_size(res));

	return base ?: IOMEM_ERR_PTR(-ENOMEM);
}

static int rtl9607dq_ssp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rtl9607dq_ssp *ssp;
	unsigned int i;
	int ret;

	ssp = devm_kzalloc(dev, sizeof(*ssp), GFP_KERNEL);
	if (!ssp)
		return -ENOMEM;

	ssp->dev = dev;
	spin_lock_init(&ssp->lock);

	ssp->base = rtl9607dq_ssp_map(pdev, "ssp");
	if (IS_ERR(ssp->base))
		return PTR_ERR(ssp->base);
	ssp->dma = rtl9607dq_ssp_map(pdev, "dma");
	if (IS_ERR(ssp->dma))
		return PTR_ERR(ssp->dma);
	ssp->queue = rtl9607dq_ssp_map(pdev, "queue");
	if (IS_ERR(ssp->queue))
		return PTR_ERR(ssp->queue);
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	for (i = 0; i < 2; i++) {
		ssp->desc[i] = dmam_alloc_coherent(dev, SSP_MAX_PERIODS *
						   sizeof(*ssp->desc[i]),
						   &ssp->desc_dma[i], GFP_KERNEL);
		if (!ssp->desc[i])
			return -ENOMEM;
	}

	/* 16-bit mono in the first two slots unless the card asks for TDM */
	ssp->slots = SSP_MAX_SLOTS;
	ssp->tx_mask = GENMASK(1, 0);
	ssp->rx_mask = GENMASK(1, 0);
	ssp->fs2slot = 1;

	rtl9607dq_ssp_stop(ssp);
	rtl9607dq_ssp_queue_stop(ssp, SNDRV_PCM_STREAM_PLAYBACK);
	rtl9607dq_ssp_queue_stop(ssp, SNDRV_PCM_STREAM_CAPTURE);
	ret = rtl9607dq_ssp_prime(ssp);
	if (ret)
		return ret;
	hrtimer_setup(&ssp->timer, rtl9607dq_ssp_poll, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL_SOFT);

	platform_set_drvdata(pdev, ssp);

	return devm_snd_soc_register_component(dev, &rtl9607dq_ssp_component,
					       &rtl9607dq_ssp_dai, 1);
}

static const struct of_device_id rtl9607dq_ssp_of_match[] = {
	{ .compatible = "realtek,rtl9607dq-ssp" },
	{ }
};
MODULE_DEVICE_TABLE(of, rtl9607dq_ssp_of_match);

static struct platform_driver rtl9607dq_ssp_driver = {
	.probe = rtl9607dq_ssp_probe,
	.driver = {
		.name = DRV_NAME,
		.of_match_table = rtl9607dq_ssp_of_match,
	},
};
module_platform_driver(rtl9607dq_ssp_driver);

MODULE_AUTHOR("Ifran Ali <khanu.i.i.k.2000@gmail.com>");
MODULE_DESCRIPTION("Realtek RTL9607DQ SSP PCM/TDM controller");
MODULE_LICENSE("GPL");
