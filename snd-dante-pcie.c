// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/debugfs.h>
#include <linux/miscdevice.h>
#include <linux/seq_file.h>
#include <linux/mutex.h>
#include <linux/compat.h>
#include <linux/iopoll.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/initval.h>

#define DRIVER_NAME    "snd-dante-pcie"
#define CARD_NAME      "DantePCIe"
#define CARD_LONGNAME  "Dante PCIe"

#define PCI_VENDOR_GENNUM   0x1a39
#define PCI_DEVICE_GN4124   0x0004

#define FPGA_REG_CTRL       0x0004
#define FPGA_EVENT_SET      0x0008
#define FPGA_EVENT_CLR      0x000c
#define FPGA_EVENT          0x0010
#define FPGA_EVENT_EN       0x0014
#define FPGA_DPTR           0x0020
#define FPGA_CSR            0x0030
#define FPGA_CSR_RUN        BIT(0)
#define FPGA_CSR_STOP       BIT(1)
#define FPGA_FW_VERSION     0x0070
#define FPGA_SAMPLE_CNT_LO  0x0074
#define FPGA_SAMPLE_CNT_HI  0x0078

#define DRAM_PRIMARY        0x4000
#define DRAM_SHADOW         0x6000

#define GN4124_INT_CTRL     0x0810
#define GN4124_INT_STAT     0x0814
#define GN4124_INT_CFG_BASE 0x0820
#define GN4124_GPIO_BYPASS  0x0a00

#define INT_STAT_INT1       BIT(5)
#define EVENT_TX_EN         BIT(2)
#define EVENT_RX_EN         BIT(3)

#define CHUNK_FRAMES        16
#define CHUNK_BYTES         (CHUNK_FRAMES * 4)
#define VDMA_TX_ACK         0x0a
#define VDMA_RX_ACK         0x0b

#define MAX_BUF_BYTES       (512 * 128 * CHUNK_BYTES)
#define MIN_BUF_BYTES       (64 * 1024)
#define DANTE_IOC_MAGIC     'D'
#define DANTE_GET_INFO      _IOR(DANTE_IOC_MAGIC, 0, struct dante_card_info)

struct dante_card_info {
	u32 firmware_version;
	u32 sample_rate;
	u32 rx_channels;
	u32 tx_channels;
	u32 period_size;
	u32 chunks_per_channel;
	u32 channel_step;
	u32 dma_running;
	u64 sample_count;
	u64 rx_buffer_phys;
	u64 tx_buffer_phys;
	u32 rx_buffer_size;
	u32 tx_buffer_size;
};

static int index = SNDRV_DEFAULT_IDX1;
static char *id = SNDRV_DEFAULT_STR1;
module_param(index, int, 0444);
module_param(id, charp, 0444);

static atomic_t card_counter = ATOMIC_INIT(0);

struct dante_pcie {
	int card_index;
	struct pci_dev *pdev;
	struct snd_card *card;
	struct snd_pcm *pcm;
	struct snd_dma_buffer buffers[2];
	void __iomem *bar0;
	void __iomem *bar4;
	unsigned long bar0_len;
	unsigned long bar4_len;
	unsigned int sample_rate;
	unsigned int max_channels;
	u32 fw_version;
	struct snd_pcm_substream *pb_sub;
	struct snd_pcm_substream *cap_sub;
	spinlock_t lock;
	struct mutex buffer_mutex;
	bool disconnected;
	bool pb_running;
	bool cap_running;
	bool configured[2];
	unsigned int period_frames[2];
	struct dentry *dbgfs;
	struct miscdevice miscdev;
	bool miscdev_registered;
	char miscdev_name[32];

	void *rx_area;
	dma_addr_t rx_addr;
	unsigned int rx_channels;
	unsigned int rx_stride;
	unsigned int rx_total;

	void *tx_area;
	dma_addr_t tx_addr;
	unsigned int tx_channels;
	unsigned int tx_stride;
	unsigned int tx_total;

	unsigned int cur_period_size;
	unsigned int cur_chunks;

};

static const struct {
	unsigned int rate;
	unsigned int max_ch;
} rate_table[] = {
	[0] = { 192000, 64 },
	[1] = { 96000, 128 },
	[2] = { 48000, 128 },
	[3] = { 0, 0 },
	[4] = { 176400, 64 },
	[5] = { 88200, 128 },
	[6] = { 44100, 128 },
};

/* Caller holds buffer_mutex (or owns the not-yet-published probe).
 * The Dante controller owns the clock. Refresh idle PCM capabilities from
 * the FPGA, and reject a changed clock while a PCM configuration exists.
 */
static int dante_refresh_format(struct dante_pcie *d)
{
	u32 firmware;
	unsigned int rate_idx;

	if (d->disconnected)
		return -ENODEV;
	firmware = ioread32(d->bar0 + FPGA_FW_VERSION);
	if (!firmware || firmware == U32_MAX)
		return -ENODEV;
	rate_idx = (firmware >> 28) & 0x7;
	if (rate_idx >= ARRAY_SIZE(rate_table) || !rate_table[rate_idx].rate)
		return -ENODEV;
	if ((d->configured[0] || d->configured[1]) &&
	    (d->sample_rate != rate_table[rate_idx].rate ||
	     d->max_channels != rate_table[rate_idx].max_ch))
		return -EBUSY;
	d->fw_version = firmware;
	d->sample_rate = rate_table[rate_idx].rate;
	d->max_channels = rate_table[rate_idx].max_ch;
	return 0;
}

static inline void dram_write(struct dante_pcie *d, u32 idx, u32 val)
{
	iowrite32(val, d->bar0 + DRAM_PRIMARY + idx * 4);
	if (idx < 0x30)
		iowrite32(val, d->bar0 + DRAM_SHADOW + idx * 4);
}

static inline u32 dram_read(struct dante_pcie *d, u32 idx)
{
	return ioread32(d->bar0 + DRAM_PRIMARY + idx * 4);
}

static const u32 vdma_microcode[] = {
	/* ENTRY: reload period countdown from word 0x08 */
	0x20000008, 0xa2000009,
	/* Acknowledge the channel gate before accessing its descriptors.
	 * A zero acknowledgement means this direction can be reprogrammed
	 * while the opposite direction continues. Inactive ring pointers must
	 * also remain untouched by the wrap checks before WAIT.
	 */
	0x20000006, 0xa2000000 | VDMA_TX_ACK, 0x1800003b,
	0x20000018, 0x1000003b,
	0x20000010, 0xa2000018,
	0x40000014, 0x5000001a,
	0x20000007, 0xa2000000 | VDMA_RX_ACK, 0x18000044,
	0x20000019, 0x10000044,
	0x20000011, 0xa2000019,
	0x40000016, 0x5000001c,
	/* VDMA_WAIT_EVENT(0x001, 0x001): vdma_seqcode.h lines 139-143, EVENT_EN=bit0 EVENT_STATE=bit0 */
	0x90001001, 0x80000001, 0x80000002,
	0x20000006,
	0x18000054, 0x4000001a,
	0xf0000003, 0xe0000012,
	0x21000002, 0x1000004a,
	0x20000018, 0x21000002, 0xa2000018,
	0x4000001a, 0xe0000005, 0x5000001a,
	0x20000007,
	0x18000061, 0x4000001c,
	0xf0000004, 0xe0000013,
	0x21000002, 0x10000057,
	0x20000019, 0x21000002, 0xa2000019,
	0x4000001c, 0xe0000005, 0x5000001c,
	0x20000009, 0x21000002, 0xa2000009,
	0x10000032,
	0x84000002,
	0x1a000030,
};

static void vdma_upload_code(struct dante_pcie *d)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(vdma_microcode); i++)
		iowrite32(vdma_microcode[i],
			  d->bar0 + DRAM_PRIMARY + (0x30 + i) * 4);
}

static void vdma_init_data(struct dante_pcie *d)
{
	unsigned int i;

	dram_write(d, 0x00, 0x1a000030);
	dram_write(d, 0x01, 0x00000001);
	dram_write(d, 0x02, 0xffffffff);
	dram_write(d, 0x03, 0x00000000);
	dram_write(d, 0x04, 0x00000000);
	dram_write(d, 0x05, CHUNK_BYTES);
	dram_write(d, 0x06, 0);
	dram_write(d, 0x07, 0);
	dram_write(d, 0x08, 32);
	dram_write(d, 0x09, 0);
	for (i = 0x0a; i <= 0x0f; i++)
		dram_write(d, i, 0);
	for (i = 0x10; i <= 0x2f; i++)
		dram_write(d, i, 0);
}

static void bridge_disable_interrupts(struct dante_pcie *d)
{
	unsigned int i;

	for (i = 0; i < 8; i++)
		iowrite32(0, d->bar4 + GN4124_INT_CFG_BASE + i * 4);
	iowrite32(0x7FF0, d->bar4 + GN4124_INT_STAT);
}

static void vdma_stop(struct dante_pcie *d)
{
	/* Do not queue another stop command on an already halted sequencer. */
	if (!(ioread32(d->bar0 + FPGA_CSR) & FPGA_CSR_RUN))
		return;
	iowrite32(FPGA_CSR_STOP, d->bar0 + FPGA_CSR);
	ioread32(d->bar0 + FPGA_CSR);
}

static int vdma_start(struct dante_pcie *d, unsigned int stream)
{
	u32 csr = ioread32(d->bar0 + FPGA_CSR);
	u32 ack;
	u32 ack_word = stream == SNDRV_PCM_STREAM_PLAYBACK ?
		VDMA_TX_ACK : VDMA_RX_ACK;
	u32 channels = stream == SNDRV_PCM_STREAM_PLAYBACK ?
		d->tx_channels : d->rx_channels;
	int err;

	/* A STOP issued while already halted remains pending until the next
	 * instruction. An older driver can leave that state across reload.
	 * RUN can read as set before that instruction retires: wait for the
	 * pending stop to complete before resuming the prepared program.
	 */
	if (csr & FPGA_CSR_STOP) {
		if (!(csr & FPGA_CSR_RUN))
			iowrite32(FPGA_CSR_RUN, d->bar0 + FPGA_CSR);
		err = read_poll_timeout_atomic(ioread32, csr,
			!(csr & (FPGA_CSR_RUN | FPGA_CSR_STOP)),
			1, 2000, false, d->bar0 + FPGA_CSR);
		if (err)
			return err;
	}
	if (!(csr & FPGA_CSR_RUN))
		iowrite32(FPGA_CSR_RUN, d->bar0 + FPGA_CSR);
	/* START is complete only after the program has seen this direction's
	 * channel gate. For an already running engine this takes at most one
	 * normal chunk cycle, and does not reset the other stream.
	 */
	err = read_poll_timeout_atomic(dram_read, ack, ack == channels,
				       1, 2000, false, d, ack_word);
	if (err)
		return err;
	return (ioread32(d->bar0 + FPGA_CSR) & FPGA_CSR_RUN) ? 0 : -EIO;
}

static void dante_quiesce(struct dante_pcie *d)
{
	iowrite32(0x0000ffff, d->bar0 + FPGA_EVENT_CLR);
	vdma_stop(d);
	bridge_disable_interrupts(d);
}

static void vdma_stop_if_idle(struct dante_pcie *d)
{
	if (!d->pb_running && !d->cap_running)
		vdma_stop(d);
}

static void bridge_init(struct dante_pcie *d)
{
	iowrite32(0x00000002, d->bar4 + GN4124_INT_CTRL);
	iowrite32(0x00000002, d->bar4 + GN4124_GPIO_BYPASS);
	/* REF 52624 p131: bits 0-1 RO, bits 2-3 write-1-to-assert SWI, bits 4-14 write-1-to-clear */
	iowrite32(0x7FF0, d->bar4 + GN4124_INT_STAT);
	iowrite32(INT_STAT_INT1, d->bar4 + GN4124_INT_CFG_BASE);
}

static irqreturn_t dante_irq(int irq, void *dev_id)
{
	struct dante_pcie *d = dev_id;
	struct snd_pcm_substream *playback, *capture;
	unsigned long flags;
	u32 stat;

	spin_lock_irqsave(&d->lock, flags);
	stat = ioread32(d->bar4 + GN4124_INT_STAT);
	if (!(stat & INT_STAT_INT1)) {
		spin_unlock_irqrestore(&d->lock, flags);
		return IRQ_NONE;
	}

	iowrite32(INT_STAT_INT1, d->bar4 + GN4124_INT_STAT);
	playback = d->pb_running ? d->pb_sub : NULL;
	capture = d->cap_running ? d->cap_sub : NULL;
	spin_unlock_irqrestore(&d->lock, flags);

	if (playback)
		snd_pcm_period_elapsed(playback);
	if (capture)
		snd_pcm_period_elapsed(capture);

	return IRQ_HANDLED;
}

static const struct snd_pcm_hardware dante_hw = {
	.info = SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_NONINTERLEAVED |
		SNDRV_PCM_INFO_JOINT_DUPLEX |
		SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats = SNDRV_PCM_FMTBIT_S32_LE,
	.channels_min = 1,
	.channels_max = 128,
	.period_bytes_min = CHUNK_BYTES,
	.period_bytes_max = MAX_BUF_BYTES / 2,
	.periods_min = 2,
	.periods_max = 512,
	.buffer_bytes_max = MAX_BUF_BYTES,
};

static int dante_period_rule(struct snd_pcm_hw_params *params,
			     struct snd_pcm_hw_rule *rule)
{
	struct snd_pcm_substream *sub = rule->private;
	struct dante_pcie *d = snd_pcm_substream_chip(sub);
	unsigned int other = sub->stream ^ 1;
	struct snd_interval interval = { .integer = 1 };
	int err = 0;

	mutex_lock(&d->buffer_mutex);
	if (d->configured[other]) {
		interval.min = d->period_frames[other];
		interval.max = interval.min;
		err = snd_interval_refine(hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_PERIOD_SIZE),
					&interval);
	}
	mutex_unlock(&d->buffer_mutex);
	return err;
}

static int dante_open(struct snd_pcm_substream *sub)
{
	struct dante_pcie *d = snd_pcm_substream_chip(sub);
	struct snd_pcm_runtime *rt = sub->runtime;
	int err;

	mutex_lock(&d->buffer_mutex);
	err = dante_refresh_format(d);
	if (err) {
		mutex_unlock(&d->buffer_mutex);
		return err;
	}
	rt->hw = dante_hw;
	rt->hw.buffer_bytes_max = d->buffers[sub->stream].bytes;
	rt->hw.period_bytes_max = rt->hw.buffer_bytes_max / 2;
	rt->hw.channels_max = d->max_channels;
	rt->hw.rates = snd_pcm_rate_to_rate_bit(d->sample_rate);
	if (!rt->hw.rates)
		rt->hw.rates = SNDRV_PCM_RATE_KNOT;
	rt->hw.rate_min = d->sample_rate;
	rt->hw.rate_max = d->sample_rate;
	mutex_unlock(&d->buffer_mutex);

	err = snd_pcm_hw_constraint_step(rt, 0, SNDRV_PCM_HW_PARAM_PERIOD_SIZE,
					  CHUNK_FRAMES);
	if (err)
		return err;
	err = snd_pcm_hw_constraint_step(rt, 0, SNDRV_PCM_HW_PARAM_BUFFER_SIZE,
					  CHUNK_FRAMES);
	if (err)
		return err;
	return snd_pcm_hw_rule_add(rt, 0, SNDRV_PCM_HW_PARAM_PERIOD_SIZE,
				  dante_period_rule, sub,
				  SNDRV_PCM_HW_PARAM_PERIOD_SIZE, -1);
}

static int dante_hw_params(struct snd_pcm_substream *sub,
			   struct snd_pcm_hw_params *params)
{
	struct dante_pcie *d = snd_pcm_substream_chip(sub);
	unsigned int stream = sub->stream;
	unsigned int other = stream ^ 1;
	int err = 0;

	mutex_lock(&d->buffer_mutex);
	err = dante_refresh_format(d);
	if (err)
		goto unlock;
	if (params_rate(params) != d->sample_rate ||
	    params_channels(params) > d->max_channels) {
		err = -EINVAL;
	} else if (params_buffer_bytes(params) > d->buffers[stream].bytes) {
		err = -ENOMEM;
	} else if (d->configured[other] &&
		   d->period_frames[other] != params_period_size(params)) {
		err = -EINVAL;
	} else {
		snd_pcm_set_runtime_buffer(sub, &d->buffers[stream]);
		sub->runtime->dma_bytes = params_buffer_bytes(params);
		d->period_frames[stream] = params_period_size(params);
		d->configured[stream] = true;
	}
unlock:
	mutex_unlock(&d->buffer_mutex);
	return err;
}

static int dante_hw_free(struct snd_pcm_substream *sub)
{
	struct dante_pcie *d = snd_pcm_substream_chip(sub);
	unsigned long flags;

	mutex_lock(&d->buffer_mutex);
	spin_lock_irqsave(&d->lock, flags);
	if (sub->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		d->pb_sub = NULL;
		d->pb_running = false;
		d->tx_area = NULL;
		d->tx_addr = 0;
		d->tx_channels = 0;
		d->tx_stride = 0;
		d->tx_total = 0;
	} else {
		d->cap_sub = NULL;
		d->cap_running = false;
		d->rx_area = NULL;
		d->rx_addr = 0;
		d->rx_channels = 0;
		d->rx_stride = 0;
		d->rx_total = 0;
	}
	d->configured[sub->stream] = false;
	d->period_frames[sub->stream] = 0;
	if (!d->configured[0] && !d->configured[1]) {
		d->cur_period_size = 0;
		d->cur_chunks = 0;
	}
	if (!d->disconnected) {
		dram_write(d, sub->stream == SNDRV_PCM_STREAM_PLAYBACK ? 0x06 : 0x07, 0);
		vdma_stop_if_idle(d);
	}
	snd_pcm_set_runtime_buffer(sub, NULL);
	spin_unlock_irqrestore(&d->lock, flags);
	mutex_unlock(&d->buffer_mutex);
	return 0;
}

static int dante_close(struct snd_pcm_substream *sub)
{
	return dante_hw_free(sub);
}

static bool dante_direction_idle(struct dante_pcie *d, unsigned int stream)
{
	unsigned long flags;
	bool idle;

	spin_lock_irqsave(&d->lock, flags);
	idle = d->disconnected || (!d->pb_running && !d->cap_running) ||
		!dram_read(d, stream == SNDRV_PCM_STREAM_PLAYBACK ?
			   VDMA_TX_ACK : VDMA_RX_ACK);
	spin_unlock_irqrestore(&d->lock, flags);
	return idle;
}

static int dante_wait_idle(struct snd_pcm_substream *sub)
{
	struct dante_pcie *d = snd_pcm_substream_chip(sub);
	bool idle;

	/* STOP clears the channel gate. Before ALSA reuses its descriptors,
	 * wait for the sequencer to observe that gate, after any old transfer.
	 * This leaves the opposite direction and shared IRQ clock running.
	 */
	return read_poll_timeout(dante_direction_idle, idle, idle, 50, 20000,
				 false, d, sub->stream);
}

static int dante_sync_stop(struct snd_pcm_substream *sub)
{
	struct dante_pcie *d = snd_pcm_substream_chip(sub);
	int err = dante_wait_idle(sub);

	synchronize_irq(d->card->sync_irq);
	return err;
}

static int dante_prepare(struct snd_pcm_substream *sub)
{
	struct dante_pcie *d = snd_pcm_substream_chip(sub);
	struct snd_pcm_runtime *rt = sub->runtime;
	unsigned int channels = rt->channels;
	unsigned int buf_frames = rt->buffer_size;
	unsigned int period_frames = rt->period_size;
	unsigned int chunks = buf_frames / CHUNK_FRAMES;
	unsigned int stride = chunks * CHUNK_BYTES;
	unsigned int periods_per_irq = period_frames / CHUNK_FRAMES;
	dma_addr_t addr = rt->dma_addr;
	unsigned long flags;
	bool engine_running;
	int err;

	/* PCM core ignores sync_stop's return value. Enforce the handshake
	 * here too, so a timeout cannot allow descriptor reuse.
	 */
	err = dante_wait_idle(sub);
	if (err) {
		dev_dbg(&d->pdev->dev,
			"prepare timeout stream=%u csr=%#x dptr=%#x ack=%#x/%#x\n",
			sub->stream, ioread32(d->bar0 + FPGA_CSR),
			ioread32(d->bar0 + FPGA_DPTR),
			dram_read(d, VDMA_TX_ACK), dram_read(d, VDMA_RX_ACK));
		return err;
	}
	mutex_lock(&d->buffer_mutex);
	err = dante_refresh_format(d);
	if (err) {
		mutex_unlock(&d->buffer_mutex);
		return err;
	}
	if (rt->rate != d->sample_rate || channels > d->max_channels) {
		mutex_unlock(&d->buffer_mutex);
		return -EINVAL;
	}
	spin_lock_irqsave(&d->lock, flags);
	if (sub->stream == SNDRV_PCM_STREAM_PLAYBACK ?
	    d->pb_running : d->cap_running) {
		spin_unlock_irqrestore(&d->lock, flags);
		mutex_unlock(&d->buffer_mutex);
		return -EBUSY;
	}

	engine_running = d->pb_running || d->cap_running;
	if (engine_running && d->cur_period_size != period_frames) {
		spin_unlock_irqrestore(&d->lock, flags);
		mutex_unlock(&d->buffer_mutex);
		return -EINVAL;
	}
	if (!engine_running) {
		vdma_stop(d);
		/* XFER_CTL: app note 53715 s4.1.1, CNT=64 bytes,
		 * bit 13=direction, bit 16=enable.
		 */
		dram_write(d, 0x03, 0x00012040);
		dram_write(d, 0x04, 0x00010040);
	}

	if (sub->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		iowrite32(EVENT_TX_EN, d->bar0 + FPGA_EVENT_CLR);
		d->pb_sub = sub;

		d->tx_area = rt->dma_area;
		d->tx_addr = addr;
		d->tx_channels = channels;
		d->tx_stride = stride;
		d->tx_total = channels * stride;

		dram_write(d, 0x06, 0);
		dram_write(d, 0x10, chunks);
		dram_write(d, 0x12, stride);
		dram_write(d, 0x14, lower_32_bits(addr));
		dram_write(d, 0x15, upper_32_bits(addr));
		dram_write(d, 0x18, chunks);
		dram_write(d, 0x1a, lower_32_bits(addr));
		dram_write(d, 0x1b, upper_32_bits(addr));
	} else {
		iowrite32(EVENT_RX_EN, d->bar0 + FPGA_EVENT_CLR);
		d->cap_sub = sub;

		d->rx_area = rt->dma_area;
		d->rx_addr = addr;
		d->rx_channels = channels;
		d->rx_stride = stride;
		d->rx_total = channels * stride;

		dram_write(d, 0x07, 0);
		dram_write(d, 0x11, chunks);
		dram_write(d, 0x13, stride);
		dram_write(d, 0x16, lower_32_bits(addr));
		dram_write(d, 0x17, upper_32_bits(addr));
		dram_write(d, 0x19, chunks);
		dram_write(d, 0x1c, lower_32_bits(addr));
		dram_write(d, 0x1d, upper_32_bits(addr));
	}

	if (!engine_running) {
		d->cur_period_size = period_frames;
		d->cur_chunks = chunks;
		dram_write(d, 0x08, periods_per_irq);
		/* Zero would wrap to U32_MAX on the first decrement. */
		dram_write(d, 0x09, periods_per_irq);
		iowrite32(0x30, d->bar0 + FPGA_DPTR);
	}

	spin_unlock_irqrestore(&d->lock, flags);
	mutex_unlock(&d->buffer_mutex);
	return 0;
}

/* Called with d->lock held, before starting or stopping the sequencer. */
static void dante_gate_direction(struct dante_pcie *d, unsigned int stream,
				 bool running)
{
	if (stream == SNDRV_PCM_STREAM_PLAYBACK) {
		dram_write(d, VDMA_TX_ACK, U32_MAX);
		dram_write(d, 0x06, running ? d->tx_channels : 0);
		WRITE_ONCE(d->pb_running, running);
		iowrite32(EVENT_TX_EN, d->bar0 +
			  (running ? FPGA_EVENT_SET : FPGA_EVENT_CLR));
	} else {
		dram_write(d, VDMA_RX_ACK, U32_MAX);
		dram_write(d, 0x07, running ? d->rx_channels : 0);
		WRITE_ONCE(d->cap_running, running);
		iowrite32(EVENT_RX_EN, d->bar0 +
			  (running ? FPGA_EVENT_SET : FPGA_EVENT_CLR));
	}
}

static int dante_trigger(struct snd_pcm_substream *sub, int cmd)
{
	struct dante_pcie *d = snd_pcm_substream_chip(sub);
	struct snd_pcm_substream *s;
	unsigned long flags;
	unsigned int streams = 0, stream;
	int err = 0;

	spin_lock_irqsave(&d->lock, flags);
	if (d->disconnected) {
		err = -ENODEV;
		goto unlock;
	}
	if (cmd != SNDRV_PCM_TRIGGER_START && cmd != SNDRV_PCM_TRIGGER_STOP) {
		err = -EINVAL;
		goto unlock;
	}

	/* ALSA holds the linked streams' locks across this callback. Program
	 * both gates before RUN for a linked duplex start: waiting for one
	 * direction first would give it a chunk's head start and misalign the
	 * shared period interrupt with the other direction's ring position.
	 * Unlinked streams still start and stop independently.
	 */
	snd_pcm_group_for_each_entry(s, sub) {
		if (s->pcm == sub->pcm) {
			streams |= BIT(s->stream);
			snd_pcm_trigger_done(s, sub);
		}
	}
	for (stream = 0; stream < 2; stream++)
		if (streams & BIT(stream))
			dante_gate_direction(d, stream,
					     cmd == SNDRV_PCM_TRIGGER_START);
	if (cmd == SNDRV_PCM_TRIGGER_START) {
		for (stream = 0; stream < 2; stream++) {
			if (!(streams & BIT(stream)))
				continue;
			err = vdma_start(d, stream);
			if (err)
				break;
		}
		if (err)
			for (stream = 0; stream < 2; stream++)
				if (streams & BIT(stream))
					dante_gate_direction(d, stream, false);
	}
	vdma_stop_if_idle(d);
unlock:
	if (!d->disconnected)
		dev_dbg(&d->pdev->dev,
			"trigger stream=%u group=%#x cmd=%d running=%u/%u csr=%#x dptr=%#x ack=%#x/%#x err=%d\n",
			sub->stream, streams, cmd, d->pb_running, d->cap_running,
			ioread32(d->bar0 + FPGA_CSR),
			ioread32(d->bar0 + FPGA_DPTR),
			dram_read(d, VDMA_TX_ACK), dram_read(d, VDMA_RX_ACK), err);
	spin_unlock_irqrestore(&d->lock, flags);

	return err;
}

static snd_pcm_uframes_t dante_pointer(struct snd_pcm_substream *sub)
{
	struct dante_pcie *d = snd_pcm_substream_chip(sub);
	u32 cur_lo, start_lo;
	unsigned int byte_off;
	unsigned long flags;

	spin_lock_irqsave(&d->lock, flags);
	if (d->disconnected) {
		spin_unlock_irqrestore(&d->lock, flags);
		return SNDRV_PCM_POS_XRUN;
	}
	if (sub->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		cur_lo = dram_read(d, 0x1a);
		start_lo = dram_read(d, 0x14);
	} else {
		cur_lo = dram_read(d, 0x1c);
		start_lo = dram_read(d, 0x16);
	}
	spin_unlock_irqrestore(&d->lock, flags);

	byte_off = cur_lo - start_lo;
	return (byte_off / 4) % sub->runtime->buffer_size;
}

static const struct snd_pcm_ops dante_pcm_ops = {
	.open      = dante_open,
	.close     = dante_close,
	.hw_params = dante_hw_params,
	.hw_free   = dante_hw_free,
	.prepare   = dante_prepare,
	.trigger   = dante_trigger,
	.sync_stop = dante_sync_stop,
	.pointer   = dante_pointer,
};

struct dante_dbgfs_bar {
	void __iomem *base;
	unsigned long len;
};

static ssize_t dbgfs_bar_read(struct file *f, char __user *buf,
			      size_t count, loff_t *ppos)
{
	struct dante_dbgfs_bar *bar = f->private_data;
	u32 val;

	if (*ppos >= bar->len || (*ppos & 3) || count < 4)
		return 0;

	val = ioread32(bar->base + *ppos);
	if (copy_to_user(buf, &val, 4))
		return -EFAULT;

	*ppos += 4;
	return 4;
}

static int dbgfs_bar_open(struct inode *inode, struct file *f)
{
	f->private_data = inode->i_private;
	return 0;
}

static const struct file_operations dbgfs_bar_fops = {
	.owner  = THIS_MODULE,
	.open   = dbgfs_bar_open,
	.read   = dbgfs_bar_read,
	.llseek = default_llseek,
};

struct dante_dbgfs_audio {
	struct dante_pcie *d;
	int is_tx;
};

static ssize_t dbgfs_audio_read(struct file *f, char __user *buf,
				size_t count, loff_t *ppos)
{
	struct dante_dbgfs_audio *a = f->private_data;
	struct dante_pcie *d = a->d;
	void *area;
	unsigned int total;
	ssize_t avail, to_copy;
	ssize_t result = 0;

	mutex_lock(&d->buffer_mutex);
	if (a->is_tx) {
		area = d->tx_area;
		total = d->tx_total;
	} else {
		area = d->rx_area;
		total = d->rx_total;
	}

	if (!area || !total || *ppos >= total)
		goto unlock;

	avail = total - *ppos;
	to_copy = min_t(ssize_t, count, avail);

	if (copy_to_user(buf, area + *ppos, to_copy)) {
		result = -EFAULT;
		goto unlock;
	}

	*ppos += to_copy;
	result = to_copy;
unlock:
	mutex_unlock(&d->buffer_mutex);
	return result;
}

static int dbgfs_audio_open(struct inode *inode, struct file *f)
{
	f->private_data = inode->i_private;
	return 0;
}

static const struct file_operations dbgfs_audio_fops = {
	.owner  = THIS_MODULE,
	.open   = dbgfs_audio_open,
	.read   = dbgfs_audio_read,
	.llseek = default_llseek,
};

static u32 compute_peak(const s32 *samples, unsigned int count)
{
	u32 peak = 0;
	unsigned int i;

	for (i = 0; i < count; i++) {
		s32 s = samples[i];
		u32 a = (s < 0) ? (u32)(-s) : (u32)s;

		if (a > peak)
			peak = a;
	}
	return peak;
}

static void format_bar(char *out, u32 peak)
{
	int bits = fls(peak);
	int filled = (bits + 3) / 4;
	int i;

	if (filled > 8)
		filled = 8;
	for (i = 0; i < 8; i++)
		out[i] = (i < filled) ? '#' : '.';
	out[8] = '\0';
}

static int meters_show(struct seq_file *s, void *v)
{
	struct dante_pcie *d = s->private;
	void *area;
	unsigned int channels, stride, ch;
	char bar[9];

	mutex_lock(&d->buffer_mutex);
	area = d->rx_area;
	channels = d->rx_channels;
	stride = d->rx_stride;

	seq_puts(s, "RX (capture):\n");
	if (area && channels && stride) {
		unsigned int samples_per_ch = stride / 4;

		for (ch = 0; ch < channels; ch++) {
			const s32 *base = (const s32 *)(area + ch * stride);
			u32 peak = compute_peak(base, samples_per_ch);

			format_bar(bar, peak);
			seq_printf(s, "%4u: %s %08x\n", ch, bar, peak);
		}
	} else {
		seq_puts(s, "  (no active stream)\n");
	}

	seq_puts(s, "TX (playback):\n");
	area = d->tx_area;
	channels = d->tx_channels;
	stride = d->tx_stride;

	if (area && channels && stride) {
		unsigned int samples_per_ch = stride / 4;

		for (ch = 0; ch < channels; ch++) {
			const s32 *base = (const s32 *)(area + ch * stride);
			u32 peak = compute_peak(base, samples_per_ch);

			format_bar(bar, peak);
			seq_printf(s, "%4u: %s %08x\n", ch, bar, peak);
		}
	} else {
		seq_puts(s, "  (no active stream)\n");
	}

	mutex_unlock(&d->buffer_mutex);
	return 0;
}

static int meters_open(struct inode *inode, struct file *f)
{
	return single_open(f, meters_show, inode->i_private);
}

static const struct file_operations dbgfs_meters_fops = {
	.owner   = THIS_MODULE,
	.open    = meters_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

static int dante_cdev_open(struct inode *inode, struct file *f)
{
	struct miscdevice *misc = f->private_data;
	struct dante_pcie *d = container_of(misc, struct dante_pcie, miscdev);
	int err;

	if (f->f_mode & FMODE_WRITE)
		return -EPERM;
	err = snd_card_file_add(d->card, f);
	if (err)
		return err;
	f->private_data = d;
	return 0;
}

static int dante_cdev_release(struct inode *inode, struct file *f)
{
	struct dante_pcie *d = f->private_data;

	return snd_card_file_remove(d->card, f);
}

static u64 dante_read_sample_count(struct dante_pcie *d)
{
	u32 high, low, check;

	do {
		high = ioread32(d->bar0 + FPGA_SAMPLE_CNT_HI);
		low = ioread32(d->bar0 + FPGA_SAMPLE_CNT_LO);
		check = ioread32(d->bar0 + FPGA_SAMPLE_CNT_HI);
	} while (high != check);
	return ((u64)high << 32) | low;
}

static long dante_cdev_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct dante_pcie *d = f->private_data;
	struct dante_card_info info;
	u32 event;
	int err;

	if (cmd != DANTE_GET_INFO)
		return -ENOTTY;

	mutex_lock(&d->buffer_mutex);
	err = dante_refresh_format(d);
	if (err) {
		mutex_unlock(&d->buffer_mutex);
		return err;
	}
	memset(&info, 0, sizeof(info));
	info.firmware_version = d->fw_version;
	info.sample_rate = d->sample_rate;
	info.rx_channels = READ_ONCE(d->rx_channels) ?: d->max_channels;
	info.tx_channels = READ_ONCE(d->tx_channels) ?: d->max_channels;
	info.period_size = READ_ONCE(d->cur_period_size);
	info.chunks_per_channel = READ_ONCE(d->cur_chunks);
	info.channel_step = info.chunks_per_channel * CHUNK_BYTES;

	event = ioread32(d->bar0 + FPGA_EVENT);
	info.dma_running = 0;
	if (event & EVENT_TX_EN)
		info.dma_running |= 1;
	if (event & EVENT_RX_EN)
		info.dma_running |= 2;

	info.sample_count = dante_read_sample_count(d);

	info.rx_buffer_phys = READ_ONCE(d->rx_addr);
	info.tx_buffer_phys = READ_ONCE(d->tx_addr);
	info.rx_buffer_size = READ_ONCE(d->rx_total);
	info.tx_buffer_size = READ_ONCE(d->tx_total);
	mutex_unlock(&d->buffer_mutex);

	if (copy_to_user((void __user *)arg, &info, sizeof(info)))
		return -EFAULT;

	return 0;
}

static int dante_cdev_mmap(struct file *f, struct vm_area_struct *vma)
{
	struct dante_pcie *d = f->private_data;
	unsigned long offset = vma->vm_pgoff;
	unsigned long size = vma->vm_end - vma->vm_start;
	struct snd_dma_buffer *buffer;
	unsigned int stream;
	int err;

	if (offset > 1)
		return -EINVAL;
	if (vma->vm_flags & (VM_WRITE | VM_EXEC))
		return -EPERM;
	/* Linux clears VM_SHARED for a MAP_SHARED mapping of an O_RDONLY fd;
	 * VM_MAYSHARE retains the requested sharing mode in that case.
	 */
	if (!(vma->vm_flags & VM_MAYSHARE))
		return -EINVAL;

	mutex_lock(&d->buffer_mutex);
	if (d->disconnected) {
		err = -ENODEV;
		goto unlock;
	}
	stream = offset ? SNDRV_PCM_STREAM_PLAYBACK : SNDRV_PCM_STREAM_CAPTURE;
	buffer = &d->buffers[stream];
	if (!buffer->area || size > buffer->bytes) {
		err = -EINVAL;
		goto unlock;
	}
	vm_flags_clear(vma, VM_MAYWRITE | VM_MAYEXEC);
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_pgoff = 0;
	err = snd_dma_buffer_mmap(buffer, vma);
unlock:
	mutex_unlock(&d->buffer_mutex);
	return err;
}

static const struct file_operations dante_cdev_fops = {
	.owner          = THIS_MODULE,
	.open           = dante_cdev_open,
	.release        = dante_cdev_release,
	.unlocked_ioctl = dante_cdev_ioctl,
	.compat_ioctl   = compat_ptr_ioctl,
	.mmap           = dante_cdev_mmap,
};

static void dante_debugfs_init(struct dante_pcie *d)
{
	struct dante_dbgfs_bar *b0, *b4;
	struct dante_dbgfs_audio *arx, *atx;

	d->dbgfs = debugfs_create_dir(d->miscdev_name, NULL);

	b0 = devm_kzalloc(&d->pdev->dev, sizeof(*b0), GFP_KERNEL);
	b4 = devm_kzalloc(&d->pdev->dev, sizeof(*b4), GFP_KERNEL);
	if (!b0 || !b4)
		return;

	b0->base = d->bar0;
	b0->len  = d->bar0_len;
	b4->base = d->bar4;
	b4->len  = d->bar4_len;

	debugfs_create_file_size("bar0", 0444, d->dbgfs, b0,
				 &dbgfs_bar_fops, d->bar0_len);
	debugfs_create_file_size("bar4", 0444, d->dbgfs, b4,
				 &dbgfs_bar_fops, d->bar4_len);

	arx = devm_kzalloc(&d->pdev->dev, sizeof(*arx), GFP_KERNEL);
	atx = devm_kzalloc(&d->pdev->dev, sizeof(*atx), GFP_KERNEL);
	if (!arx || !atx)
		return;

	arx->d = d;
	arx->is_tx = 0;
	atx->d = d;
	atx->is_tx = 1;

	debugfs_create_file("audio_rx", 0444, d->dbgfs, arx,
			    &dbgfs_audio_fops);
	debugfs_create_file("audio_tx", 0444, d->dbgfs, atx,
			    &dbgfs_audio_fops);

	debugfs_create_file("meters", 0444, d->dbgfs, d,
			    &dbgfs_meters_fops);
}


static int dante_cdev_register(struct dante_pcie *d)
{
	int err;

	d->miscdev.minor = MISC_DYNAMIC_MINOR;
	d->miscdev.name  = d->miscdev_name;
	d->miscdev.fops  = &dante_cdev_fops;
	d->miscdev.mode  = 0400;
	d->miscdev.parent = &d->pdev->dev;

	err = misc_register(&d->miscdev);
	if (!err)
		d->miscdev_registered = true;
	return err;
}

static void dante_interfaces_remove(void *data)
{
	struct dante_pcie *d = data;

	if (d->miscdev_registered) {
		misc_deregister(&d->miscdev);
		d->miscdev_registered = false;
	}
	debugfs_remove_recursive(d->dbgfs);
	d->dbgfs = NULL;
}

static void dante_free_buffers(struct snd_card *card)
{
	struct dante_pcie *d = card->private_data;
	unsigned int stream;

	for (stream = 0; stream < ARRAY_SIZE(d->buffers); stream++) {
		if (d->buffers[stream].area)
			snd_dma_free_pages(&d->buffers[stream]);
		memset(&d->buffers[stream], 0, sizeof(d->buffers[stream]));
	}
}

static int dante_alloc_buffers(struct dante_pcie *d)
{
	u64 original_mask = d->pdev->dev.coherent_dma_mask;
	u64 masks[] = { original_mask, DMA_BIT_MASK(32) };
	size_t bytes;
	unsigned int stream, mask, mask_count = original_mask > DMA_BIT_MASK(32) ? 2 : 1;
	int err = -ENOMEM;

	/* Keep both buffers for the card lifetime, including diagnostic VMAs.
	 * Try balanced pairs so one large allocation cannot starve the other
	 * direction on a long-running, fragmented host.
	 * The low DMA zone may still contain large blocks when normal memory
	 * is fragmented. The bridge already supports 32-bit DMA addresses.
	 */
	for (bytes = MAX_BUF_BYTES; bytes >= MIN_BUF_BYTES; bytes >>= 1) {
		for (mask = 0; mask < mask_count; mask++) {
			if (dma_set_coherent_mask(&d->pdev->dev, masks[mask]))
				continue;
			for (stream = 0; stream < ARRAY_SIZE(d->buffers); stream++) {
				err = snd_dma_alloc_pages(SNDRV_DMA_TYPE_DEV, &d->pdev->dev,
							  bytes, &d->buffers[stream]);
				if (err)
					break;
			}
			if (!err) {
				dev_info(&d->pdev->dev,
					 "fixed DMA buffers: %zu bytes per direction, mask 0x%llx\n",
					 bytes, masks[mask]);
				return 0;
			}
			dante_free_buffers(d->card);
			if (err != -ENOMEM)
				goto failed;
		}
	}
failed:
	dma_set_coherent_mask(&d->pdev->dev, original_mask);
	return err;
}

static int dante_probe_card(struct pci_dev *pdev)
{
	struct dante_pcie *d;
	struct snd_card *card;
	struct snd_pcm *pcm;
	int err;

	err = pcim_enable_device(pdev);
	if (err)
		return err;

	err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (err) {
		err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
		if (err)
			return err;
	}

	if (!(pci_resource_flags(pdev, 0) & IORESOURCE_MEM) ||
	    !(pci_resource_flags(pdev, 4) & IORESOURCE_MEM) ||
	    pci_resource_len(pdev, 0) < DRAM_SHADOW + 0x30 * 4 ||
	    pci_resource_len(pdev, 4) < GN4124_GPIO_BYPASS + 4)
		return -ENODEV;

	err = pcim_iomap_regions(pdev, BIT(0) | BIT(4), DRIVER_NAME);
	if (err)
		return err;

	err = snd_devm_card_new(&pdev->dev, index, id ?: CARD_NAME,
				THIS_MODULE, sizeof(*d), &card);
	if (err)
		return err;

	d = card->private_data;
	d->card_index = atomic_fetch_add(1, &card_counter);
	d->pdev = pdev;
	d->card = card;
	card->private_free = dante_free_buffers;
	spin_lock_init(&d->lock);
	mutex_init(&d->buffer_mutex);

	d->bar0 = pcim_iomap_table(pdev)[0];
	d->bar4 = pcim_iomap_table(pdev)[4];
	if (!d->bar0 || !d->bar4)
		return -ENOMEM;

	d->bar0_len = pci_resource_len(pdev, 0);
	d->bar4_len = pci_resource_len(pdev, 4);

	err = dante_refresh_format(d);
	if (err)
		return dev_err_probe(&pdev->dev, err, "unreadable or unknown FPGA format\n");
	dev_info(&pdev->dev, "firmware 0x%08x, rate %u Hz, max %u channels\n",
		 d->fw_version, d->sample_rate, d->max_channels);

	pci_set_drvdata(pdev, d);
	dante_quiesce(d);

	err = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI);
	if (err < 0)
		return dev_err_probe(&pdev->dev, err,
				     "failed to allocate one MSI vector\n");

	iowrite32(0x0000001f, d->bar0 + FPGA_REG_CTRL);
	ioread32(d->bar0 + FPGA_REG_CTRL);
	iowrite32(0x00000007, d->bar0 + FPGA_REG_CTRL);
	iowrite32(0x0000ffff, d->bar0 + FPGA_EVENT_EN);

	vdma_upload_code(d);
	vdma_init_data(d);
	vdma_stop(d);

	err = devm_request_irq(&pdev->dev, pci_irq_vector(pdev, 0),
			       dante_irq, 0, CARD_NAME, d);
	if (err)
		return err;

	card->sync_irq = pci_irq_vector(pdev, 0);

	strscpy(card->driver, CARD_NAME, sizeof(card->driver));
	strscpy(card->shortname, CARD_NAME, sizeof(card->shortname));
	snprintf(card->longname, sizeof(card->longname),
		 "%s at %s", CARD_LONGNAME, pci_name(pdev));

	err = snd_pcm_new(card, CARD_NAME " PCM", 0, 1, 1, &pcm);
	if (err)
		return err;

	pcm->private_data = d;
	d->pcm = pcm;
	strscpy(pcm->name, CARD_NAME " PCM", sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &dante_pcm_ops);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &dante_pcm_ops);
	err = dante_alloc_buffers(d);
	if (err)
		return err;

	pci_set_master(pdev);
	bridge_init(d);

	err = snd_card_register(card);
	if (err)
		return err;

	snprintf(d->miscdev_name, sizeof(d->miscdev_name),
		 "dante-pcie%d", d->card_index);

	dante_debugfs_init(d);

	err = dante_cdev_register(d);
	if (err)
		dev_warn(&pdev->dev, "failed to register chardev: %d\n", err);

	return devm_add_action_or_reset(&pdev->dev, dante_interfaces_remove, d);
}

static void dante_shutdown(struct pci_dev *pdev)
{
	struct dante_pcie *d = pci_get_drvdata(pdev);
	unsigned long flags;

	if (d) {
		mutex_lock(&d->buffer_mutex);
		spin_lock_irqsave(&d->lock, flags);
		d->disconnected = true;
		d->pb_running = false;
		d->cap_running = false;
		dante_quiesce(d);
		spin_unlock_irqrestore(&d->lock, flags);
		mutex_unlock(&d->buffer_mutex);
	}
	pci_clear_master(pdev);
}

static int dante_probe(struct pci_dev *pdev, const struct pci_device_id *pci_id)
{
	int err = dante_probe_card(pdev);

	if (err) {
		struct dante_pcie *d = pci_get_drvdata(pdev);

		if (d) {
			dante_shutdown(pdev);
			dante_interfaces_remove(d);
		}
		snd_card_free_on_error(&pdev->dev, err);
		pci_set_drvdata(pdev, NULL);
	}
	return err;
}

static void dante_remove(struct pci_dev *pdev)
{
	struct dante_pcie *d = pci_get_drvdata(pdev);

	dante_shutdown(pdev);
	if (d) {
		snd_card_disconnect(d->card);
		dante_interfaces_remove(d);
	}
}

static const struct pci_device_id dante_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_GENNUM, PCI_DEVICE_GN4124) },
	{ }
};
MODULE_DEVICE_TABLE(pci, dante_ids);

static struct pci_driver dante_driver = {
	.name    = DRIVER_NAME,
	.id_table = dante_ids,
	.probe   = dante_probe,
	.remove  = dante_remove,
	.shutdown = dante_shutdown,
};
module_pci_driver(dante_driver);

MODULE_DESCRIPTION("ALSA driver for Dante PCIe audio cards");
MODULE_AUTHOR("Christopher Ritsen");
MODULE_LICENSE("GPL");
