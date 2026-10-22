// SPDX-License-Identifier: GPL-2.0
/*
 * zrn-wlbt: the first stages of Wi-Fi and Bluetooth on the Galaxy M20: boot the
 * Exynos 7904's wireless subsystem (Samsung SCSC "Maxwell", S612 radio) and
 * see its firmware run, before any of Samsung's SCSC stack is ported.
 *
 * The subsystem has its own Cortex-R4 (and an M4), boots from a 4 MB window of
 * DRAM (reserved in the DT as wifibt_if at 0xe9000000) and talks to the AP
 * through two mailboxes. The sequence is Samsung's (drivers/misc/samsung/scsc
 * in its M20 kernel: mxman.c, platform_mif.c, mif_reg_S5E7885.h):
 *
 *   1. mx140.bin to the start of the window, the rest cleared
 *   2. both mailboxes cleared and masked; ISSR0 = the firmware's entry point,
 *      ISSR1 = its config block (an offset in the window), ISSR2 = 0xbcdeedcb,
 *      ISSR3 = start-up flags
 *   3. PMU: boot from DRAM, the window's base and size, power on, reset off,
 *      start
 *
 * Before step 2 the driver lays out what mxman.c's transports_init() does in
 * the window after the firmware's runtime area: the management transport (a
 * ring each way), the R4's and M4's debugger rings and the firmware's log
 * ring, described by the config block (struct mxconf) whose offset goes to
 * ISSR1. A firmware that accepts it answers MM_START_IND on the management
 * ring, with the RF chip's revision.
 *
 * Nothing touches the hardware at probe. Writing 1 to the "boot" attribute
 * runs the sequence and waits up to 2 s for MM_START_IND; "boot" reads back
 * what was seen (also the mailbox interrupts and whether the panic record the
 * header names was written). "mxlog" shows the newest words of the firmware's
 * log (raw: decoding needs Samsung's log-strings.bin), "firmware" the image's
 * build id. Writing 0 sends MM_HALT_REQ to a started firmware, then puts the
 * subsystem back into reset as Samsung's driver stops it.
 *
 * Not here yet: the service layer (SM_MSG_START_REQ on the management
 * transport's channel 1), which Wi-Fi (HIP4) and Bluetooth (BSMHCP, notes in
 * devices/m20lte/notes/wlbt-bluetooth.md) start on top of this.
 *
 * Before the first boot the window is opened to the subsystem in the TZASC
 * (an EL3 call, once per boot, as Samsung's probe does).
 *
 * Mainline has neither of what Samsung's kernel also does around reset release:
 * exynos_acpm_set_flag() (ACPM sets BUCK2's voltage) and the modem's vote for
 * the shared regulator (exynos_pmu_shared_reg_enable()). If the core does not
 * start, those are the first suspects. Both exist for this SoC in the Galaxy
 * Tab A 10.1 2019 mainline work (github.com/Mollomm1/linux-exynos7885, branch
 * gta3xl: the Exynos 7885 ACPM driver's set_wlbt_flag and
 * exynos-scsc-shared-rail.c), which is where to take them from.
 *
 * The firmware is the stock one, from the VENDOR partition's etc/wifi/mx140.bin,
 * installed as /lib/firmware/scsc/mx140.bin.
 *
 * Bound by the device tree (compatible "zerone,m20-wlbt", in
 * devices/m20lte/kernel/exynos7904-m20lte.dts).
 */
#include <linux/arm-smccc.h>
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/sizes.h>
#include <linux/unaligned.h>

#define FW_NAME			"scsc/mx140.bin"

/* mailbox (each of the R4's and the M4's) */
#define MBOX_INTCR0		0x00c
#define MBOX_INTMR0		0x010
#define MBOX_INTSR0		0x014
#define MBOX_INTMSR0		0x018
#define MBOX_INTGR1		0x01c
#define MBOX_INTCR1		0x020
#define MBOX_INTMR1		0x024
#define MBOX_ISSR(n)		(0x080 + 4 * (n))
#define MBOX_COUNT		8
#define MBOX2_MAGIC		0xbcdeedcb

/* PMU */
#define WIFI_CTRL_NS		0x0140
#define  WIFI_PWRON		BIT(1)
#define  WIFI_RESET_SET		BIT(2)
#define  WIFI_RESET_REQ_CLR	BIT(8)
#define WIFI_CTRL_S		0x0144
#define  WIFI_START		BIT(3)
#define WIFI_STAT		0x0148
#define CENTRAL_SEQ_WIFI_CFG	0x0380
#define CENTRAL_SEQ_WIFI_STATUS	0x0384
#define  SEQ_STATES		GENMASK(23, 16)
#define  SEQ_IN_RESET		0x80
#define RESET_AHEAD_WIFI	0x1360
#define CLEANY_BUS_WIFI		0x1364
#define LOGIC_RESET_WIFI	0x1368
#define TCXO_GATE_WIFI		0x136c
#define WIFI_DISABLE_ISO	0x1370
#define WIFI_RESET_ISO		0x1374
#define WIFI2AP_MEM_CONFIG0	0x7300		/* window size, 4 KB units */
#define WIFI2AP_MEM_CONFIG1	0x7304		/* window base >> 12 */
#define WIFI2AP_MEM_CONFIG2	0x7308
#define WIFI2AP_MEM_CONFIG3	0x730c
#define WLBT_BOOT_TEST_RST_CFG	0x7330		/* 0: boot from DRAM */

/* EL3 call that opens the window to the subsystem in the TZASC (mif_reg_S5E7885.h) */
#define SMC_WLBT_TZASC		0x82000710
#define WLBT_TZASC		0

/* the mx140.bin header ("smxf"), FWHDR_02 in Samsung's fwhdr.c */
#define FWHDR_MAGIC_OFF		8
#define FWHDR_RUNTIME_LEN_OFF	36
#define FWHDR_ENTRY_OFF		40
#define FWHDR_BUILD_ID_OFF	48
#define FWHDR_BUILD_ID_LEN	128
#define FWHDR_R4_PANIC_OFF	176
#define FWHDR_MIN_LEN		188

/*
 * Stage 2: the infrastructure config block (Samsung's mxconf.h), so the
 * firmware finds its management transport and says it has started.
 *
 * Every *_loc is an offset in the window (the R4 sees the window at 0). A
 * stream is a ring of fixed-size packets plus a read and a write index, each
 * owned by one side, and a pair of mailbox interrupt bits: the writer raises
 * write_bit after writing, the reader raises read_bit after reading. To-AP bits
 * are the upper half of the R4 mailbox's INTMSR0/INTCR0/INTMR0, from-AP bits
 * are raised in its INTGR1. Bit 0 from the AP is reserved (the firmware's
 * panic request).
 */
#define MXCONF_MAGIC		0x79828486
#define MXCONF_VERSION_MAJOR	0
#define MXCONF_VERSION_MINOR	1

struct mxcbufconf {
	__le32 buffer_loc;
	__le32 num_packets;
	__le32 packet_size;
	__le32 read_index_loc;
	__le32 write_index_loc;
};

struct mxstreamconf {
	struct mxcbufconf buf_conf;
	u8 read_bit_idx;
	u8 write_bit_idx;
};

struct mxtransconf {
	struct mxstreamconf to_ap;
	struct mxstreamconf from_ap;
};

struct mxconf {
	__le32 magic;
	__le16 version_major;
	__le16 version_minor;
	struct mxtransconf mgmt;
	struct mxtransconf gdb_r4;
	struct mxtransconf gdb_m4;
	struct mxstreamconf mxlog;
};
static_assert(sizeof(struct mxstreamconf) == 24);
static_assert(sizeof(struct mxconf) == 176);

/* management messages: a channel byte and 7 bytes of payload */
#define MGMT_PACKET		8
#define MGMT_PACKETS		(512 / MGMT_PACKET)
#define MGMT_CHAN_MAXWELL	0
#define MM_START_IND		0
#define MM_HALT_REQ		1
#define MM_HALT_RSP		6
#define GDB_PACKETS		(2048 / 4)
#define MXLOG_PACKETS		(16384 / 4)
#define MXLOG_KEEP		256		/* words of firmware log kept for sysfs */
#define HEAP_BLOCK		64		/* miframman's block */
#define INTR_BITS		16

enum { TARGET_R4, TARGET_M4 };

struct mx_stream {
	u32 buf, ridx, widx;		/* window offsets */
	u32 num, size;
	u8 read_bit, write_bit;
	bool in;			/* to the AP */
};

struct zrn_wlbt {
	struct device *dev;
	void __iomem *r4;		/* the R4's mailbox */
	void __iomem *m4;		/* the M4's */
	struct regmap *pmu;
	phys_addr_t mem_phys;
	size_t mem_size;
	void *mem;			/* the window, write-combined */
	struct mutex lock;
	bool running;
	bool tzasc_done;		/* EL3 refuses a second grant in one boot */
	u32 panic_off;
	atomic_t mbox_irqs, wdog_irqs;
	char seen[256];
	char build_id[FWHDR_BUILD_ID_LEN];

	/* stage 2 */
	u32 heap;			/* next free offset in the window */
	u16 tohost_used, fromhost_used[2];
	struct mx_stream mgmt_in, mgmt_out, gdb_r4_in, gdb_r4_out,
			 gdb_m4_in, gdb_m4_out, mxlog;
	struct completion started, halted;
	int mbox_irq, wdog_irq;		/* enabled only while running */
	u32 rf_hw_ver;
	spinlock_t log_lock;
	u32 log[MXLOG_KEEP];
	unsigned int log_head, log_count;
};

static u32 *win32(struct zrn_wlbt *w, u32 off)
{
	return w->mem + off;
}

static u32 heap_alloc(struct zrn_wlbt *w, u32 bytes)
{
	u32 off = w->heap;

	if (off + bytes > w->mem_size)
		return 0;
	w->heap = ALIGN(off + bytes, HEAP_BLOCK);
	return off;
}

/* the bit numbers Samsung's mifintrbit hands out, in the same order */
static int intr_alloc(u16 *map, int first)
{
	int bit;

	for (bit = first; bit < INTR_BITS; bit++)
		if (!(*map & BIT(bit))) {
			*map |= BIT(bit);
			return bit;
		}
	return -ENOSPC;
}

/* mif_stream_init(): reserved = the from-AP panic bit instead of a fresh one */
static int stream_init(struct zrn_wlbt *w, struct mx_stream *s, bool in, int target,
		       u32 num, u32 size, bool reserved)
{
	int to_ap, from_ap;

	s->num = num;
	s->size = size;
	s->in = in;
	s->buf = heap_alloc(w, num * size);
	s->ridx = heap_alloc(w, 4);
	s->widx = heap_alloc(w, 4);
	if (!s->buf || !s->ridx || !s->widx)
		return -ENOMEM;
	*win32(w, s->ridx) = 0;
	*win32(w, s->widx) = 0;

	to_ap = intr_alloc(&w->tohost_used, 0);
	from_ap = reserved ? 0 : intr_alloc(&w->fromhost_used[target], 1);
	if (to_ap < 0 || from_ap < 0)
		return -ENOSPC;

	if (in) {
		memset(w->mem + s->buf, 0xff, num * size);
		s->read_bit = from_ap;
		s->write_bit = to_ap;
	} else {
		s->read_bit = to_ap;
		s->write_bit = from_ap;
	}
	return 0;
}

static void stream_conf(const struct mx_stream *s, struct mxstreamconf *c)
{
	c->buf_conf.buffer_loc = cpu_to_le32(s->buf);
	c->buf_conf.num_packets = cpu_to_le32(s->num);
	c->buf_conf.packet_size = cpu_to_le32(s->size);
	c->buf_conf.read_index_loc = cpu_to_le32(s->ridx);
	c->buf_conf.write_index_loc = cpu_to_le32(s->widx);
	c->read_bit_idx = s->read_bit;
	c->write_bit_idx = s->write_bit;
}

/* The to-AP bit this stream's peer raises, as seen in INTMSR0 >> 16 */
static u32 stream_tohost_bit(const struct mx_stream *s)
{
	return BIT(s->in ? s->write_bit : s->read_bit);
}

/* Lay out every transport and the config block; returns the block's offset */
static int mxconf_build(struct zrn_wlbt *w, u32 runtime)
{
	struct mxconf *conf;
	u32 off;
	int ret = 0;

	w->heap = ALIGN(runtime, HEAP_BLOCK);
	w->tohost_used = 0;
	w->fromhost_used[TARGET_R4] = BIT(0);	/* MIFINTRBIT_RESERVED_PANIC_R4 */
	w->fromhost_used[TARGET_M4] = BIT(0);

	/* transports_init()'s order, so the bits match what the firmware is used to */
	ret = ret ?: stream_init(w, &w->mgmt_in, true, TARGET_R4, MGMT_PACKETS, MGMT_PACKET, false);
	ret = ret ?: stream_init(w, &w->mgmt_out, false, TARGET_R4, MGMT_PACKETS, MGMT_PACKET, false);
	ret = ret ?: stream_init(w, &w->gdb_r4_in, true, TARGET_R4, GDB_PACKETS, 4, false);
	ret = ret ?: stream_init(w, &w->gdb_r4_out, false, TARGET_R4, GDB_PACKETS, 4, true);
	ret = ret ?: stream_init(w, &w->gdb_m4_in, true, TARGET_M4, GDB_PACKETS, 4, false);
	ret = ret ?: stream_init(w, &w->gdb_m4_out, false, TARGET_M4, GDB_PACKETS, 4, true);
	ret = ret ?: stream_init(w, &w->mxlog, true, TARGET_R4, MXLOG_PACKETS, 4, false);
	if (ret)
		return ret;

	off = heap_alloc(w, sizeof(*conf));
	if (!off)
		return -ENOMEM;
	conf = w->mem + off;
	memset(conf, 0, sizeof(*conf));
	conf->magic = cpu_to_le32(MXCONF_MAGIC);
	conf->version_major = cpu_to_le16(MXCONF_VERSION_MAJOR);
	conf->version_minor = cpu_to_le16(MXCONF_VERSION_MINOR);
	stream_conf(&w->mgmt_in, &conf->mgmt.to_ap);
	stream_conf(&w->mgmt_out, &conf->mgmt.from_ap);
	stream_conf(&w->gdb_r4_in, &conf->gdb_r4.to_ap);
	stream_conf(&w->gdb_r4_out, &conf->gdb_r4.from_ap);
	stream_conf(&w->gdb_m4_in, &conf->gdb_m4.to_ap);
	stream_conf(&w->gdb_m4_out, &conf->gdb_m4.from_ap);
	stream_conf(&w->mxlog, &conf->mxlog);
	return off;
}

/* Raise a from-AP bit, as platform_mif_irq_bit_set() (the M4's goes to its own mailbox) */
static void raise_bit(struct zrn_wlbt *w, int target, u8 bit)
{
	writel(BIT(bit), (target == TARGET_M4 ? w->m4 : w->r4) + MBOX_INTGR1);
}

/* Next packet of an IN stream, or NULL; consume with stream_pop() */
static const u8 *stream_peek(struct zrn_wlbt *w, struct mx_stream *s)
{
	u32 r = READ_ONCE(*win32(w, s->ridx)), wr = READ_ONCE(*win32(w, s->widx));

	if (r == wr || r >= s->num)
		return NULL;
	rmb();
	return w->mem + s->buf + r * s->size;
}

static void stream_pop(struct zrn_wlbt *w, struct mx_stream *s)
{
	u32 r = READ_ONCE(*win32(w, s->ridx));

	mb();
	WRITE_ONCE(*win32(w, s->ridx), (r + 1) % s->num);
}

/* Tell the writer there is room again, once per batch of pops */
static void stream_read_done(struct zrn_wlbt *w, struct mx_stream *s)
{
	wmb();
	raise_bit(w, TARGET_R4, s->read_bit);
}

/* One packet into an OUT stream (mif_stream_write()) */
static int stream_push(struct zrn_wlbt *w, struct mx_stream *s, const void *pkt)
{
	u32 r = READ_ONCE(*win32(w, s->ridx)), wr = READ_ONCE(*win32(w, s->widx));

	if ((wr + 1) % s->num == r)
		return -ENOSPC;
	memcpy(w->mem + s->buf + wr * s->size, pkt, s->size);
	wmb();
	WRITE_ONCE(*win32(w, s->widx), (wr + 1) % s->num);
	wmb();
	raise_bit(w, TARGET_R4, s->write_bit);
	return 0;
}

static void mgmt_drain(struct zrn_wlbt *w)
{
	const u8 *p;
	bool any = false;

	while ((p = stream_peek(w, &w->mgmt_in))) {
		any = true;
		/* struct mxmgr_message { u8 channel; struct ma_msg_packet { u8 msg; u32 arg; } } */
		u8 chan = p[0], msg = p[1];
		u32 arg = get_unaligned_le32(p + 2);

		if (chan != MGMT_CHAN_MAXWELL)
			dev_info(w->dev, "management message on channel %u: %*ph\n", chan, MGMT_PACKET, p);
		else if (msg == MM_START_IND) {
			w->rf_hw_ver = arg;
			dev_info(w->dev, "firmware started (MM_START_IND), RF hardware version %#06x\n", arg);
			complete(&w->started);
		} else if (msg == MM_HALT_RSP) {
			complete(&w->halted);
		} else {
			dev_info(w->dev, "firmware message %u, arg %#x\n", msg, arg);
		}
		stream_pop(w, &w->mgmt_in);
	}
	if (any)
		stream_read_done(w, &w->mgmt_in);
}

/* The firmware's log: words keyed to its log-strings.bin, kept raw */
static void mxlog_drain(struct zrn_wlbt *w)
{
	const u8 *p;
	bool any = false;

	while ((p = stream_peek(w, &w->mxlog))) {
		any = true;
		spin_lock(&w->log_lock);
		w->log[w->log_head] = get_unaligned_le32(p);
		w->log_head = (w->log_head + 1) % MXLOG_KEEP;
		w->log_count = min(w->log_count + 1, MXLOG_KEEP);
		spin_unlock(&w->log_lock);
		stream_pop(w, &w->mxlog);
	}
	if (any)
		stream_read_done(w, &w->mxlog);
}

static void mbox_reset(struct zrn_wlbt *w)
{
	int i;

	for (i = 0; i < MBOX_COUNT; i++) {
		writel(0, w->r4 + MBOX_ISSR(i));
		writel(0, w->m4 + MBOX_ISSR(i));
	}
	/* everything masked, then everything cleared */
	writel(0xffff0000, w->r4 + MBOX_INTMR0);
	writel(0x0000ffff, w->r4 + MBOX_INTMR1);
	writel(0x0000ffff, w->m4 + MBOX_INTMR1);
	writel(0xffff0000, w->r4 + MBOX_INTCR0);
	writel(0x0000ffff, w->r4 + MBOX_INTCR1);
	writel(0x0000ffff, w->m4 + MBOX_INTCR1);
}

/* Threaded: the streams live in the window and are drained here */
static irqreturn_t zrn_wlbt_mbox_irq(int irq, void *data)
{
	struct zrn_wlbt *w = data;
	u32 pending = readl(w->r4 + MBOX_INTMSR0) >> 16;
	u32 acks;

	/* clear first, so a packet written meanwhile raises it again */
	writel(pending << 16, w->r4 + MBOX_INTCR0);
	if (atomic_inc_return(&w->mbox_irqs) <= 4)
		dev_info(w->dev, "mailbox interrupt from the firmware: %#x\n", pending);
	if (!READ_ONCE(w->running))
		return IRQ_HANDLED;

	if (pending & stream_tohost_bit(&w->mgmt_in))
		mgmt_drain(w);
	if (pending & stream_tohost_bit(&w->mxlog))
		mxlog_drain(w);

	/* "I read your packet" acks: unused, masked from now on as Samsung does */
	acks = pending & (stream_tohost_bit(&w->mgmt_out) | stream_tohost_bit(&w->gdb_r4_out) |
			  stream_tohost_bit(&w->gdb_m4_out));
	if (acks)
		writel(readl(w->r4 + MBOX_INTMR0) | acks << 16, w->r4 + MBOX_INTMR0);
	return IRQ_HANDLED;
}

static irqreturn_t zrn_wlbt_wdog_irq(int irq, void *data)
{
	struct zrn_wlbt *w = data;

	atomic_inc(&w->wdog_irqs);
	dev_warn(w->dev, "reset request from the subsystem (its watchdog)\n");
	regmap_update_bits(w->pmu, WIFI_CTRL_NS, WIFI_RESET_REQ_CLR, WIFI_RESET_REQ_CLR);
	return IRQ_HANDLED;
}

/* Back into reset, as platform_mif_pmu_reset(.., 2) and platform_mif_reset(.., 1) */
static int zrn_wlbt_stop(struct zrn_wlbt *w)
{
	u8 halt[MGMT_PACKET] = { MGMT_CHAN_MAXWELL, MM_HALT_REQ };
	u32 val;
	int ret;

	/* ask a started firmware to park first (send_mm_msg_stop_blocking()); older ones never answer */
	if (completion_done(&w->started)) {
		reinit_completion(&w->halted);
		if (!stream_push(w, &w->mgmt_out, halt) &&
		    !wait_for_completion_timeout(&w->halted, msecs_to_jiffies(100)))
			dev_info(w->dev, "no MM_HALT_RSP, stopping anyway\n");
	}
	WRITE_ONCE(w->running, false);
	disable_irq(w->mbox_irq);
	disable_irq(w->wdog_irq);
	mbox_reset(w);

	regmap_update_bits(w->pmu, RESET_AHEAD_WIFI, 0x3, 0);
	regmap_update_bits(w->pmu, CLEANY_BUS_WIFI, 0x1, 0);
	regmap_update_bits(w->pmu, LOGIC_RESET_WIFI, 0x3, 0);
	regmap_update_bits(w->pmu, TCXO_GATE_WIFI, 0x1, 0);
	regmap_update_bits(w->pmu, WIFI_DISABLE_ISO, 0x1, 1);
	regmap_update_bits(w->pmu, WIFI_RESET_ISO, 0x1, 0);
	regmap_update_bits(w->pmu, CENTRAL_SEQ_WIFI_CFG, BIT(16), 0);
	regmap_update_bits(w->pmu, WIFI_CTRL_NS, WIFI_RESET_SET, WIFI_RESET_SET);

	ret = regmap_read_poll_timeout(w->pmu, CENTRAL_SEQ_WIFI_STATUS, val,
				       FIELD_GET(SEQ_STATES, val) == SEQ_IN_RESET,
				       1000, 500000);
	if (ret)
		dev_err(w->dev, "the sequencer did not reach reset: status %#x\n", val);

	regmap_write(w->pmu, WIFI2AP_MEM_CONFIG0, 0);
	regmap_write(w->pmu, WIFI2AP_MEM_CONFIG1, 0);
	regmap_write(w->pmu, WIFI2AP_MEM_CONFIG2, 0);
	regmap_write(w->pmu, WIFI2AP_MEM_CONFIG3, 0);
	reinit_completion(&w->started);
	return ret;
}

static int zrn_wlbt_boot(struct zrn_wlbt *w)
{
	const struct firmware *fw;
	u32 entry, runtime, ctrl, stat, seq;
	u8 before[64];
	bool started, panicked;
	long left;
	int ret, conf_off;

	regmap_read(w->pmu, WIFI_CTRL_NS, &ctrl);
	if (ctrl & WIFI_PWRON)
		return -EBUSY;

	ret = request_firmware(&fw, FW_NAME, w->dev);
	if (ret)
		return dev_err_probe(w->dev, ret, "no %s (VENDOR's etc/wifi/mx140.bin)\n", FW_NAME);
	if (fw->size < FWHDR_MIN_LEN || memcmp(fw->data + FWHDR_MAGIC_OFF, "smxf", 4)) {
		ret = -EINVAL;
		dev_err(w->dev, "%s: not a Maxwell firmware image\n", FW_NAME);
		goto out;
	}
	entry = get_unaligned_le32(fw->data + FWHDR_ENTRY_OFF);
	runtime = get_unaligned_le32(fw->data + FWHDR_RUNTIME_LEN_OFF);
	w->panic_off = get_unaligned_le32(fw->data + FWHDR_R4_PANIC_OFF);
	if (fw->size > w->mem_size || runtime >= w->mem_size ||
	    w->panic_off + sizeof(before) > w->mem_size) {
		ret = -E2BIG;
		goto out;
	}
	strscpy(w->build_id, fw->data + FWHDR_BUILD_ID_OFF, sizeof(w->build_id));

	/* 1: the image, then the heap after its runtime area as miframman_init() lays it */
	memset(w->mem, 0, w->mem_size);
	memcpy(w->mem, fw->data, fw->size);
	memcpy(before, w->mem + w->panic_off, sizeof(before));
	conf_off = mxconf_build(w, runtime);
	if (conf_off < 0) {
		ret = conf_off;
		goto out;
	}
	dev_info(w->dev, "%s: %s, entry %#x, runtime %#x, config at %#x, heap used to %#x\n",
		 FW_NAME, w->build_id, entry, runtime, conf_off, w->heap);

	/* 2 */
	mbox_reset(w);
	writel(entry, w->r4 + MBOX_ISSR(0));
	writel(conf_off, w->r4 + MBOX_ISSR(1));
	writel(MBOX2_MAGIC, w->r4 + MBOX_ISSR(2));
	writel(0, w->r4 + MBOX_ISSR(3));
	/* let the firmware's interrupts to the AP through for the bits handed out */
	writel(~((u32)w->tohost_used << 16), w->r4 + MBOX_INTMR0);
	wmb();

	/*
	 * Without the TZASC grant the R4 runs but its stores to the window never
	 * land (seen on the Galaxy Tab A 10.1 2019, the same SoC). Samsung makes
	 * the call once at probe and carries on whatever EL3 answers.
	 */
	if (!w->tzasc_done) {
		struct arm_smccc_res res;

		arm_smccc_smc(SMC_WLBT_TZASC, WLBT_TZASC, w->mem_phys, w->mem_size,
			      0, 0, 0, 0, &res);
		w->tzasc_done = true;
		dev_info(w->dev, "TZASC grant for the window: EL3 answered %#lx\n", res.a0);
	}

	/* 3 */
	atomic_set(&w->mbox_irqs, 0);
	atomic_set(&w->wdog_irqs, 0);
	reinit_completion(&w->started);
	w->rf_hw_ver = 0;
	w->log_head = w->log_count = 0;
	WRITE_ONCE(w->running, true);
	enable_irq(w->mbox_irq);
	enable_irq(w->wdog_irq);
	regmap_write(w->pmu, WLBT_BOOT_TEST_RST_CFG, 0);
	regmap_write(w->pmu, WIFI2AP_MEM_CONFIG1, (w->mem_phys & 0xffffc000) >> 12);
	regmap_write(w->pmu, WIFI2AP_MEM_CONFIG0, w->mem_size >> 12);
	/* the second window (BT-ABOX shared memory): the M20 has none, as in Samsung's tree */
	regmap_write(w->pmu, WIFI2AP_MEM_CONFIG2, 0);
	regmap_write(w->pmu, WIFI2AP_MEM_CONFIG3, 0);
	regmap_update_bits(w->pmu, WIFI_CTRL_NS, WIFI_PWRON, WIFI_PWRON);
	regmap_update_bits(w->pmu, WIFI_CTRL_NS, WIFI_RESET_SET, 0);
	regmap_update_bits(w->pmu, WIFI_CTRL_S, WIFI_START, WIFI_START);

	/* 4: MM_START_IND within Samsung's 2 s (mm_completion_timeout_ms) */
	left = wait_for_completion_timeout(&w->started, msecs_to_jiffies(2000));
	started = left > 0;
	panicked = memcmp(before, w->mem + w->panic_off, sizeof(before));
	regmap_read(w->pmu, WIFI_STAT, &stat);
	regmap_read(w->pmu, CENTRAL_SEQ_WIFI_STATUS, &seq);
	snprintf(w->seen, sizeof(w->seen),
		 "%s after %u ms: RF hardware %#06x, mailbox irqs %d, watchdog irqs %d, panic record %s, "
		 "WIFI_STAT %#x, sequencer %#lx, firmware log %u words\n",
		 started ? "started" : panicked || atomic_read(&w->mbox_irqs) ? "alive, no MM_START_IND" :
		 "no sign of life", jiffies_to_msecs(msecs_to_jiffies(2000) - left), w->rf_hw_ver,
		 atomic_read(&w->mbox_irqs), atomic_read(&w->wdog_irqs),
		 panicked ? "written" : "untouched", stat, FIELD_GET(SEQ_STATES, seq), w->log_count);
	dev_info(w->dev, "%s", w->seen);
	if (panicked)
		dev_info(w->dev, "panic record: %*ph\n", 32, w->mem + w->panic_off);
out:
	release_firmware(fw);
	return ret;
}

static ssize_t boot_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct zrn_wlbt *w = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s", w->seen[0] ? w->seen : "not booted\n");
}

static ssize_t boot_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct zrn_wlbt *w = dev_get_drvdata(dev);
	bool on;
	int ret;

	ret = kstrtobool(buf, &on);
	if (ret)
		return ret;
	mutex_lock(&w->lock);
	if (on)
		ret = w->running ? -EBUSY : zrn_wlbt_boot(w);
	else
		ret = w->running ? zrn_wlbt_stop(w) : 0;
	mutex_unlock(&w->lock);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(boot);

/* The newest words of the firmware's log, oldest first, for decoding offline */
static ssize_t mxlog_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct zrn_wlbt *w = dev_get_drvdata(dev);
	unsigned int i, n, start;
	ssize_t len = 0;

	spin_lock_irq(&w->log_lock);
	n = w->log_count;
	start = (w->log_head + MXLOG_KEEP - n) % MXLOG_KEEP;
	for (i = 0; i < n; i++)
		len += sysfs_emit_at(buf, len, "%08x%c", w->log[(start + i) % MXLOG_KEEP],
				     i % 8 == 7 || i == n - 1 ? '\n' : ' ');
	spin_unlock_irq(&w->log_lock);
	return len;
}
static DEVICE_ATTR_RO(mxlog);

static ssize_t firmware_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct zrn_wlbt *w = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", w->build_id[0] ? w->build_id : "not loaded");
}
static DEVICE_ATTR_RO(firmware);

static struct attribute *zrn_wlbt_attrs[] = {
	&dev_attr_boot.attr,
	&dev_attr_mxlog.attr,
	&dev_attr_firmware.attr,
	NULL
};
ATTRIBUTE_GROUPS(zrn_wlbt);

static int zrn_wlbt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct reserved_mem *rmem;
	struct device_node *np;
	struct zrn_wlbt *w;
	int irq, ret;

	w = devm_kzalloc(dev, sizeof(*w), GFP_KERNEL);
	if (!w)
		return -ENOMEM;
	w->dev = dev;
	mutex_init(&w->lock);
	spin_lock_init(&w->log_lock);
	init_completion(&w->started);
	init_completion(&w->halted);
	platform_set_drvdata(pdev, w);

	w->r4 = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(w->r4))
		return PTR_ERR(w->r4);
	w->m4 = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(w->m4))
		return PTR_ERR(w->m4);

	w->pmu = syscon_regmap_lookup_by_phandle(dev->of_node, "samsung,syscon-phandle");
	if (IS_ERR(w->pmu))
		return dev_err_probe(dev, PTR_ERR(w->pmu), "no PMU\n");

	np = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!np)
		return dev_err_probe(dev, -ENODEV, "no memory-region\n");
	rmem = of_reserved_mem_lookup(np);
	of_node_put(np);
	if (!rmem)
		return dev_err_probe(dev, -ENODEV, "memory-region is not reserved\n");
	w->mem_phys = rmem->base;
	w->mem_size = rmem->size;
	w->mem = devm_memremap(dev, w->mem_phys, w->mem_size, MEMREMAP_WC);
	if (IS_ERR(w->mem))
		return dev_err_probe(dev, PTR_ERR(w->mem), "cannot map the window\n");

	irq = platform_get_irq_byname(pdev, "mbox");
	if (irq < 0)
		return irq;
	w->mbox_irq = irq;
	/* off until a boot is asked for: whatever the subsystem left pending stays quiet */
	ret = devm_request_threaded_irq(dev, irq, NULL, zrn_wlbt_mbox_irq,
					IRQF_ONESHOT | IRQF_NO_AUTOEN, "zrn-wlbt-mbox", w);
	if (ret)
		return ret;
	irq = platform_get_irq_byname(pdev, "wdog");
	if (irq < 0)
		return irq;
	w->wdog_irq = irq;
	ret = devm_request_irq(dev, irq, zrn_wlbt_wdog_irq, IRQF_NO_AUTOEN, "zrn-wlbt-wdog", w);
	if (ret)
		return ret;

	dev_info(dev, "Maxwell window %pa, %zu MB; boot with: echo 1 > %s/boot\n",
		 &w->mem_phys, w->mem_size >> 20, dev_name(dev));
	return 0;
}

static void zrn_wlbt_remove(struct platform_device *pdev)
{
	struct zrn_wlbt *w = platform_get_drvdata(pdev);

	mutex_lock(&w->lock);
	if (w->running)
		zrn_wlbt_stop(w);
	mutex_unlock(&w->lock);
}

static const struct of_device_id zrn_wlbt_of_match[] = {
	{ .compatible = "zerone,m20-wlbt" },
	{ }
};
MODULE_DEVICE_TABLE(of, zrn_wlbt_of_match);

static struct platform_driver zrn_wlbt_driver = {
	.driver = {
		.name = "zrn-wlbt",
		.of_match_table = zrn_wlbt_of_match,
		.dev_groups = zrn_wlbt_groups,
	},
	.probe = zrn_wlbt_probe,
	.remove = zrn_wlbt_remove,
};
module_platform_driver(zrn_wlbt_driver);

MODULE_DESCRIPTION("Galaxy M20: boot the Maxwell Wi-Fi/BT subsystem's firmware");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE(FW_NAME);
