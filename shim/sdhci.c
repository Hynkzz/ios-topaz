/*
 * ginkgo (Redmi Note 8) probe: can we talk to the eMMC that ABL booted from?
 *
 * The Note 8 has no UFS; its storage is eMMC behind a Qualcomm SDHCI host
 * (sdhci@4744000, "sdhc_1"). This probe runs on the gold CPU with the MMU off,
 * dumps the SDHCI registers ABL left behind, and if the host looks alive it
 * issues READ-ONLY commands in PIO mode (no DMA at all):
 *   CMD13 SEND_STATUS   - find the card's RCA and check it is in TRAN state
 *   CMD17 READ_SINGLE   - LBA 1, must start with "EFI PART"
 *   CMD18 READ_MULTIPLE - the GPT entries, look for "userdata"
 * v2: ABL leaves the host with the SD clock off, bus power off and a 1-bit bus,
 * so the probe re-initializes the card (CMD0/1/2/3/7, then CMD6 to switch the
 * bus to 8-bit high speed). CMD6 only changes the card's volatile bus settings
 * (EXT_CSD BUS_WIDTH/HS_TIMING); no user data is ever written: there is no
 * CMD24/CMD25 in this file.
 *
 * Register layout: SD Host Controller Simplified Specification 3.00.
 * Every register access is 32 bits wide.
 */
#include <stdint.h>
#include "console.h"
#include "sdhci.h"

#define HC_BASE		0x04744000UL	/* sdhci@4744000 "hc_mem", ginkgo live FDT */
#define CQ_BASE		0x04745000UL	/* "cqhci_mem" (command queue engine) */

#define R_BLOCK		0x04	/* [15:0] block size, [31:16] block count */
#define R_ARG		0x08
#define R_XFER_CMD	0x0c	/* [15:0] transfer mode, [31:16] command */
#define R_RESP0		0x10
#define R_DATA		0x20
#define R_PRESENT	0x24
#define R_HOSTCTL	0x28
#define R_CLOCK		0x2c	/* [15:0] clock, [23:16] timeout, [31:24] soft reset */
#define R_INT_STAT	0x30	/* [15:0] normal, [31:16] error */
#define R_INT_EN	0x34
#define R_SIG_EN	0x38
#define R_CTRL2		0x3c	/* [31:16] host control 2 */
#define R_CAPS		0x40
#define R_CAPS1		0x44
#define R_VERSION	0xfc	/* [31:16] host version */

#define CQ_CFG		0x08

/* sdhci-msm v5 vendor power-control handshake (offsets from hc_mem) */
#define R_PWRCTL_STATUS	0x240
#define R_PWRCTL_CLEAR	0x248
#define R_PWRCTL_CTL	0x24c

#define PS_CMD_INHIBIT	(1u << 0)
#define PS_DAT_INHIBIT	(1u << 1)

#define INT_CMD_DONE	(1u << 0)
#define INT_XFER_DONE	(1u << 1)
#define INT_BUF_RD	(1u << 5)
#define INT_ERROR	(1u << 15)

/* Command register: index << 8 | flags */
#define C_RESP136	0x01
#define C_RESP48	0x02
#define C_RESP48B	0x03
#define C_CRC		0x08
#define C_INDEX		0x10
#define C_DATA		0x20

/* Transfer mode */
#define M_DMA		(1u << 0)
#define M_BLKCNT	(1u << 1)
#define M_ACMD12	(1u << 2)
#define M_READ		(1u << 4)
#define M_MULTI		(1u << 5)

#define TIMEOUT_TICKS	(19200000u / 2)		/* 0.5 s at 19.2 MHz */

/* The eMMC host by default; test_sdcard() points this at the microSD host. */
static uintptr_t hc_base = HC_BASE;

static inline uint32_t hc_rd(uint32_t off) { return *(volatile uint32_t *)(hc_base + off); }
static inline void hc_wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(hc_base + off) = v; }

static uint32_t buf[32 * 512 / 4];	/* up to 32 sectors */

static uint64_t ticks(void)
{
	uint64_t t;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
	return t;
}

/* Wait for any bit of mask (or an error) in the interrupt status. */
static int wait_int(uint32_t mask, uint32_t *st)
{
	uint64_t end = ticks() + TIMEOUT_TICKS;
	uint32_t s;
	while (!((s = hc_rd(R_INT_STAT)) & (mask | INT_ERROR))) {
		if (ticks() > end) {
			*st = s;
			return -1;
		}
	}
	*st = s;
	return (s & INT_ERROR) ? -2 : 0;
}

static void soft_reset_cmd_dat(void)
{
	hc_wr(R_CLOCK, hc_rd(R_CLOCK) | (6u << 24));	/* reset CMD and DAT lines */
	uint64_t end = ticks() + TIMEOUT_TICKS;
	while ((hc_rd(R_CLOCK) >> 24) & 6)
		if (ticks() > end)
			break;
	hc_wr(R_INT_STAT, 0xffffffff);
}

static int wait_idle(uint32_t bits)
{
	uint64_t end = ticks() + TIMEOUT_TICKS;
	while (hc_rd(R_PRESENT) & bits)
		if (ticks() > end)
			return -1;
	return 0;
}

/* Issue one command. For data commands, reads nblk sectors into buf (PIO). */
static int cmd(uint32_t idx, uint32_t arg, uint32_t flags, uint32_t mode, uint32_t nblk,
	       uint32_t *resp, uint32_t *st)
{
	*st = 0;
	if (wait_idle(PS_CMD_INHIBIT | (nblk ? PS_DAT_INHIBIT : 0)))
		return -3;
	hc_wr(R_INT_STAT, 0xffffffff);
	if (nblk)
		hc_wr(R_BLOCK, (nblk << 16) | 512);
	hc_wr(R_ARG, arg);
	hc_wr(R_XFER_CMD, ((idx << 8 | flags) << 16) | mode);
	int r = wait_int(INT_CMD_DONE, st);
	if (r)
		return r;
	hc_wr(R_INT_STAT, INT_CMD_DONE);
	*resp = hc_rd(R_RESP0);
	for (uint32_t b = 0; b < nblk; b++) {
		r = wait_int(INT_BUF_RD, st);
		if (r)
			return r;
		hc_wr(R_INT_STAT, INT_BUF_RD);
		for (int w = 0; w < 128; w++)
			buf[b * 128 + w] = hc_rd(R_DATA);
	}
	if (nblk) {
		r = wait_int(INT_XFER_DONE, st);
		if (r)
			return r;
		hc_wr(R_INT_STAT, INT_XFER_DONE);
	}
	return 0;
}

static uint64_t le64(const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--)
		v = v << 8 | p[i];
	return v;
}

static void delay_us(uint32_t us)
{
	uint64_t end = ticks() + (uint64_t)us * 192 / 10;
	while (ticks() < end)
		;
}

/*
 * The msm host raises a power-control request whenever bus power or a full
 * reset is written and waits for software to acknowledge it (Linux:
 * sdhci_msm_handle_pwr_irq). The regulators are already on here, so just
 * report success for whatever was requested.
 */
static uint32_t pwrctl_ack(void)
{
	uint64_t end = ticks() + TIMEOUT_TICKS / 5;
	uint32_t st;
	while (!(st = hc_rd(R_PWRCTL_STATUS)))
		if (ticks() > end)
			return 0;
	hc_wr(R_PWRCTL_CLEAR, st);
	uint32_t ok = 0;
	if (st & 3)		/* BUS_OFF / BUS_ON */
		ok |= 1;
	if (st & 0xc)		/* IO_LOW / IO_HIGH */
		ok |= 4;
	hc_wr(R_PWRCTL_CTL, ok);
	return st;
}

/* SD clock = base / (2 * div); div 0 = base. */
static void set_clock(uint32_t div)
{
	uint32_t c = hc_rd(R_CLOCK) & ~0xffffu;
	hc_wr(R_CLOCK, c | (div & 0xff) << 8 | ((div >> 8) & 3) << 6 | 1);
	uint64_t end = ticks() + TIMEOUT_TICKS;
	while (!(hc_rd(R_CLOCK) & 2))
		if (ticks() > end)
			break;
	hc_wr(R_CLOCK, hc_rd(R_CLOCK) | 4);
	delay_us(1000);
}

/* Wait for the end of a busy signal on DAT0 (R1b commands). */
static void wait_busy(void)
{
	uint32_t st;
	(void)wait_int(INT_XFER_DONE, &st);
	hc_wr(R_INT_STAT, 0xffffffff);
	uint64_t end = ticks() + TIMEOUT_TICKS;
	while (!(hc_rd(R_PRESENT) & (1u << 20)))
		if (ticks() > end)
			break;
}

static int read_lba1(const char *label)
{
	uint32_t resp, st;
	int e = cmd(17, 1, C_RESP48 | C_CRC | C_INDEX | C_DATA, M_BLKCNT | M_READ, 1, &resp, &st);
	if (e) {
		con_hex2(label, "CMD17 e=", (uint32_t)e, "st=", st);
		soft_reset_cmd_dat();
		return -1;
	}
	const uint8_t *b = (const uint8_t *)buf;
	int ok = b[0] == 'E' && b[1] == 'F' && b[2] == 'I' && b[3] == ' ' &&
		 b[4] == 'P' && b[5] == 'A' && b[6] == 'R' && b[7] == 'T';
	con_hex2(label, ok ? "EFI PART w0=" : "no GPT w0=", buf[0], "w1=", buf[1]);
	return ok ? 0 : -1;
}

/*
 * v3: 32-bit ADMA2 reads (the PIO path above costs ~1.5 us per 32-bit word,
 * far too slow for an OS). MMU off, so this memory is Device and coherent.
 * Descriptor: u16 attr (valid|end|act=tran), u16 length, u32 address.
 */
#define R_ADMA_ADDR	0x58
#define R_ADMA_ADDR_HI	0x5c
#define INT_ADMA_ERR	(1u << 25)
#define ADMA_CHUNK	0x8000u
#define BIG_SECTORS	2048u			/* 1 MiB */

struct adma_desc { uint16_t attr, len; uint32_t addr; };
static struct adma_desc adma_tbl[BIG_SECTORS * 512 / ADMA_CHUNK] __attribute__((aligned(8)));
/* 1 MiB scratch in free DDR (System RAM 0x80000000+; nothing else runs here). */
#define big ((uint8_t *)0x90000000UL)

static int adma_read(uint32_t lba, uint32_t nblk, uint8_t *dst, uint32_t *st)
{
	uint32_t bytes = nblk * 512, n = 0;
	for (uint32_t off = 0; off < bytes; off += ADMA_CHUNK, n++) {
		uint32_t len = bytes - off < ADMA_CHUNK ? bytes - off : ADMA_CHUNK;
		adma_tbl[n].attr = 0x21 | (off + len >= bytes ? 2 : 0);
		adma_tbl[n].len = (uint16_t)len;
		adma_tbl[n].addr = (uint32_t)(uintptr_t)(dst + off);
	}
	__asm__ volatile("dsb sy" ::: "memory");
	*st = 0;
	if (wait_idle(PS_CMD_INHIBIT | PS_DAT_INHIBIT))
		return -3;
	hc_wr(R_HOSTCTL, (hc_rd(R_HOSTCTL) & ~(3u << 3)) | (2u << 3));	/* 32-bit ADMA2 */
	hc_wr(R_ADMA_ADDR, (uint32_t)(uintptr_t)adma_tbl);
	hc_wr(R_ADMA_ADDR_HI, 0);
	hc_wr(R_INT_STAT, 0xffffffff);
	hc_wr(R_BLOCK, (nblk << 16) | 512);
	hc_wr(R_ARG, lba);
	hc_wr(R_XFER_CMD, ((18u << 8 | C_RESP48 | C_CRC | C_INDEX | C_DATA) << 16) |
	      M_DMA | M_BLKCNT | M_ACMD12 | M_READ | M_MULTI);
	int r = wait_int(INT_XFER_DONE, st);
	hc_wr(R_INT_STAT, 0xffffffff);
	hc_wr(R_HOSTCTL, hc_rd(R_HOSTCTL) & ~(3u << 3));
	return r;
}

static uint32_t sum32(const void *p, uint32_t bytes)
{
	const uint32_t *w = p;
	uint32_t s = 0;
	for (uint32_t i = 0; i < bytes / 4; i++)
		s = s * 31 + w[i];
	return s;
}

static void test_adma(uint32_t entry_lba, uint32_t userdata_first)
{
	uint32_t st;
	/* Same 32 GPT-entry sectors as the PIO read: the checksums must match. */
	uint32_t pio_sum = sum32(buf, 32 * 512);
	int e = adma_read(entry_lba, 32, big, &st);
	if (e) {
		con_hex2("ADMA", "failed e=", (uint32_t)e, "st=", st);
		soft_reset_cmd_dat();
		return;
	}
	uint32_t dma_sum = sum32(big, 32 * 512);
	con_hex2("ADMA", "pio sum=", pio_sum, "dma sum=", dma_sum);
	if (pio_sum != dma_sum) {
		con_str("ADMA", "MISMATCH: DMA data differs from PIO");
		return;
	}
	/* 1 MiB from the start of userdata, timed. */
	uint64_t t0 = ticks();
	e = adma_read(userdata_first, BIG_SECTORS, big, &st);
	uint32_t us = (uint32_t)((ticks() - t0) * 10 / 192);
	if (e) {
		con_hex2("ADMA 1M", "failed e=", (uint32_t)e, "st=", st);
		soft_reset_cmd_dat();
		return;
	}
	con_hex2("ADMA 1M", "KiB/s=", us ? BIG_SECTORS * 512u / us * 1000u : 0, "us=", us);
	con_str("ADMA", "OK: DMA reads work");
}

void test_sdhci(void)
{
	con_str("EMMC", "ginkgo sdhci probe v2 (no data writes)");
	uint32_t ver = hc_rd(R_VERSION);
	con_hex2("HC", "ver=", ver, "present=", hc_rd(R_PRESENT));
	con_hex2("HC", "hostctl=", hc_rd(R_HOSTCTL), "clock=", hc_rd(R_CLOCK));
	con_hex2("HC", "pwrctl=", hc_rd(R_PWRCTL_STATUS), "caps=", hc_rd(R_CAPS));
	if (ver == 0 || ver == 0xffffffff) {
		con_str("EMMC", "registers read as 0/ff: host clocks off?");
		return;
	}
	if (*(volatile uint32_t *)(CQ_BASE + CQ_CFG) & 1) {
		con_str("EMMC", "command queue engine is ON: stopping");
		return;
	}
	uint32_t base_mhz = (hc_rd(R_CAPS) >> 8) & 0xff;
	if (!base_mhz)
		base_mhz = 200;

	hc_wr(R_INT_EN, 0xffff00ff & ~0x100u);
	hc_wr(R_SIG_EN, 0);
	hc_wr(R_INT_STAT, 0xffffffff);

	/* Bus power on (1.8 V signalling setting), acknowledged to the msm core. */
	uint32_t hc = hc_rd(R_HOSTCTL);
	if (!(hc & (1u << 8))) {
		hc_wr(R_HOSTCTL, (hc & ~0xff00u) | (0x0bu << 8));
		con_hex2("POWER", "on, pwrctl=", pwrctl_ack(), "hostctl=", hc_rd(R_HOSTCTL));
	}
	/* Data timeout: max. Identification clock: 400 kHz. */
	hc_wr(R_CLOCK, (hc_rd(R_CLOCK) & ~(0xffu << 16)) | (0x0eu << 16));
	set_clock(base_mhz * 1000 / 800);		/* base / (2 * div) = 400 kHz */
	con_hex2("CLOCK", "400k reg=", hc_rd(R_CLOCK), "present=", hc_rd(R_PRESENT));

	/* Is the card still selected from ABL? (diagnostic only) */
	uint32_t resp = 0, st = 0, rca = 0;
	for (uint32_t r = 1; r <= 4 && !rca; r++) {
		if (cmd(13, r << 16, C_RESP48 | C_CRC | C_INDEX, 0, 0, &resp, &st) == 0)
			rca = r;
		else
			soft_reset_cmd_dat();
	}
	if (rca)
		con_hex2("CMD13", "rca=", rca, "state=", (resp >> 9) & 0xf);
	else
		con_hex2("CMD13", "no answer st=", st, "rca=", 0);

	/* Full identification: CMD0, CMD1 until ready, CMD2, CMD3 (RCA 1), CMD7. */
	(void)cmd(0, 0, 0, 0, 0, &resp, &st);
	delay_us(2000);
	uint32_t ocr = 0;
	for (int i = 0; i < 1000; i++) {
		if (cmd(1, 0x40ff8080, C_RESP48, 0, 0, &ocr, &st) != 0) {
			soft_reset_cmd_dat();
			ocr = 0;
		}
		if (ocr & (1u << 31))
			break;
		delay_us(1000);
	}
	con_hex2("CMD1", "ocr=", ocr, "st=", st);
	if (!(ocr & (1u << 31))) {
		con_str("EMMC", "card did not power up (no OCR ready)");
		return;
	}
	if (cmd(2, 0, C_RESP136 | C_CRC, 0, 0, &resp, &st)) {
		con_hex2("CMD2", "failed st=", st, "", 0);
		return;
	}
	con_hex2("CID", "w0=", resp, "w1=", hc_rd(R_RESP0 + 4));
	if (cmd(3, 1u << 16, C_RESP48 | C_CRC | C_INDEX, 0, 0, &resp, &st) ||
	    cmd(7, 1u << 16, C_RESP48B | C_CRC | C_INDEX, 0, 0, &resp, &st)) {
		con_hex2("CMD3/7", "failed st=", st, "resp=", resp);
		return;
	}
	wait_busy();
	(void)cmd(13, 1u << 16, C_RESP48 | C_CRC | C_INDEX, 0, 0, &resp, &st);
	con_hex2("SELECTED", "rca=", 1, "state=", (resp >> 9) & 0xf);

	if (read_lba1("LBA1 slow"))
		return;

	/* 8-bit bus, high speed (52 MHz SDR): EXT_CSD BUS_WIDTH=2, HS_TIMING=1. */
	if (cmd(6, 3u << 24 | 183u << 16 | 2u << 8, C_RESP48B | C_CRC | C_INDEX, 0, 0, &resp, &st) == 0) {
		wait_busy();
		hc_wr(R_HOSTCTL, hc_rd(R_HOSTCTL) | (1u << 5));		/* 8-bit */
	} else {
		soft_reset_cmd_dat();
	}
	if (cmd(6, 3u << 24 | 185u << 16 | 1u << 8, C_RESP48B | C_CRC | C_INDEX, 0, 0, &resp, &st) == 0) {
		wait_busy();
		hc_wr(R_HOSTCTL, hc_rd(R_HOSTCTL) | (1u << 2));		/* high speed */
		set_clock(base_mhz / 100);			/* base / (2 * div) = 50 MHz */
	} else {
		soft_reset_cmd_dat();
	}
	con_hex2("FAST", "hostctl=", hc_rd(R_HOSTCTL), "clock=", hc_rd(R_CLOCK));
	if (read_lba1("LBA1 fast"))
		return;

	const uint8_t *b = (const uint8_t *)buf;
	uint64_t entry_lba = le64(b + 72);
	uint32_t nent = b[80] | b[81] << 8 | b[82] << 16 | (uint32_t)b[83] << 24;
	con_hex2("GPT", "entries@", (uint32_t)entry_lba, "n=", nent);

	/* CMD18 + auto CMD12: the GPT entries, 4 per sector, until "userdata". */
	uint64_t t0 = ticks();
	uint32_t sectors = 0;
	for (uint32_t chunk = 0; chunk * 128 < nent && chunk < 8; chunk++) {
		int e = cmd(18, (uint32_t)entry_lba + chunk * 32, C_RESP48 | C_CRC | C_INDEX | C_DATA,
			M_BLKCNT | M_ACMD12 | M_READ | M_MULTI, 32, &resp, &st);
		if (e) {
			con_hex2("CMD18", "failed e=", (uint32_t)e, "st=", st);
			soft_reset_cmd_dat();
			return;
		}
		sectors += 32;
		for (int i = 0; i < 128; i++) {
			const uint8_t *ent = b + i * 128;
			const char *want = "userdata";
			int c = 0;
			while (want[c] && ent[56 + 2 * c] == (uint8_t)want[c] && ent[57 + 2 * c] == 0)
				c++;
			if (want[c] || ent[56 + 2 * c] != 0)
				continue;
			uint32_t us = (uint32_t)((ticks() - t0) * 10 / 192);
			uint64_t first = le64(ent + 32), end = le64(ent + 40);
			con_hex2("USERDATA", "first=", (uint32_t)first, "last=", (uint32_t)end);
			con_hex2("SPEED", "KiB/s=", us ? sectors * 512u * 1000u / us : 0, "us=", us);
			con_str("EMMC", "OK: eMMC readable from our code");
			test_adma((uint32_t)entry_lba + chunk * 32, (uint32_t)first);
			return;
		}
	}
	con_str("GPT", "no partition named userdata");
}

/*
 * microSD probe (sdhc_2, sdhci@4784000): can our code use the card slot?
 *
 * ABL boots from the eMMC and may leave sdhc_2 unclocked; an access to an
 * unclocked host can hang the bus, so its GCC clock branches are read first
 * and, if needed, enabled with the root clock on XO (19.2 MHz). The card is
 * then identified the SD way (CMD0, CMD8, ACMD41, CMD2, CMD3 with the RCA the
 * card publishes, CMD7) in a 1-bit bus and LBA 0 is read in PIO. Nothing is
 * written to the card: there is no CMD24/CMD25 here. The card's supplies
 * (vdd L22, vdd-io L5) belong to the RPM; this probe cannot switch them, it
 * only reports whether the card answers.
 * Sources: trinket.dtsi sdhc_2 / trinket-qrd.dtsi, Linux gcc-sm6125.c.
 */
#define SD_BASE			0x04784000UL
#define GCC_BASE		0x01400000UL
#define GCC_SDCC2_APPS_CBCR	0x1e004
#define GCC_SDCC2_AHB_CBCR	0x1e008
#define GCC_SDCC2_RCG_CMD	0x1e00c
#define GCC_SDCC2_RCG_CFG	0x1e010
#define CBCR_CLK_OFF		(1u << 31)
#define RCG_ROOT_OFF		(1u << 31)
#define RCG_UPDATE		(1u << 0)

static inline uint32_t gcc_rd(uint32_t off) { return *(volatile uint32_t *)(GCC_BASE + off); }
static inline void gcc_wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(GCC_BASE + off) = v; }

/* Enable a branch clock; 0 once its CLK_OFF bit clears. */
static int gcc_branch_on(uint32_t off)
{
	gcc_wr(off, gcc_rd(off) | 1);
	uint64_t end = ticks() + TIMEOUT_TICKS;
	while (gcc_rd(off) & CBCR_CLK_OFF)
		if (ticks() > end)
			return -1;
	return 0;
}

/*
 * The card's supplies, pm6125 L5 (I/O) and L22 (card), are RPM resources
 * ("ldoa" 5 and 22; ginkgo dtbo: L22 2.95 V, L5 1.8-2.95 V; allowed ranges
 * 1.648-3.104 V and 2.944-3.304 V). Requests go over GLINK in the RPM's
 * message RAM: a table of contents ("grt0") in its last 256 bytes names two
 * FIFOs, "ap2r" (to the RPM) and "r2ap", each a tail word, a head word and
 * the data; the RPM is rung through the APCS IPC register. ABL's UEFI may
 * leave its "rpm_requests" channel open: rpm_find() only reads the FIFO
 * history to find it, rpm_ldo_on() then queues one request behind it.
 * Layout: Linux drivers/rpmsg/qcom_glink_rpm.c, qcom_glink_native.c,
 * drivers/soc/qcom/smd-rpm.c; DT memory@045F0000, mailbox@0F111000.
 */
#define RPM_MSG_RAM		0x045f0000UL
#define RPM_MSG_RAM_SIZE	0x7000u
#define RPM_TOC_MAGIC		0x67727430	/* "grt0" */
#define RPM_FIFO_AP2R		0x61703272	/* "ap2r" */
#define RPM_FIFO_R2AP		0x72326170	/* "r2ap" */
#define RPM_IPC_DOORBELL	0x0f111008UL
#define GLINK_OPEN		2
#define GLINK_CLOSE		3
#define GLINK_OPEN_ACK		4
#define GLINK_INTENT		5
#define GLINK_TX_DATA		9

struct rpm_fifo { uint32_t off, size; };
static struct rpm_fifo rpm_tx, rpm_rx;

static inline uint32_t mr_rd(uint32_t off) { return *(volatile uint32_t *)(RPM_MSG_RAM + off); }
static inline void mr_wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(RPM_MSG_RAM + off) = v; }

/* Our local channel id of "rpm_requests" in what ABL left, or -1. */
static int rpm_find(void)
{
	uint32_t toc = RPM_MSG_RAM_SIZE - 256, n = mr_rd(toc + 4);
	rpm_tx.size = rpm_rx.size = 0;
	if (mr_rd(toc) != RPM_TOC_MAGIC || n > 20) {
		con_hex2("RPM", "toc magic=", mr_rd(toc), "n=", n);
		return -1;
	}
	for (uint32_t i = 0; i < n; i++) {
		uint32_t id = mr_rd(toc + 8 + 12 * i), off = mr_rd(toc + 12 + 12 * i), size = mr_rd(toc + 16 + 12 * i);
		struct rpm_fifo *f = id == RPM_FIFO_AP2R ? &rpm_tx : id == RPM_FIFO_R2AP ? &rpm_rx : 0;
		if (f && off + size <= RPM_MSG_RAM_SIZE) {
			f->off = off;
			f->size = size;
		}
	}
	if (!rpm_tx.size || !rpm_rx.size) {
		con_str("RPM", "toc without ap2r/r2ap");
		return -1;
	}
	uint32_t head = mr_rd(rpm_tx.off + 4);
	con_hex2("RPM ap2r", "tail=", mr_rd(rpm_tx.off), "head=", head);
	con_hex2("RPM r2ap", "tail=", mr_rd(rpm_rx.off), "head=", mr_rd(rpm_rx.off + 4));
	/* Walk what was sent since boot: find OPEN "rpm_requests"; count data packets. */
	int lcid = -1;
	uint32_t at = 0, data = rpm_tx.off + 8, sent = 0, ldo = 0;
	while (at + 8 <= head && head <= rpm_tx.size) {
		uint32_t w0 = mr_rd(data + at), cmd = w0 & 0xffff, p1 = w0 >> 16, len = 8;
		if (cmd == GLINK_OPEN) {
			uint32_t nl = mr_rd(data + at + 4) & 0xffff;
			const volatile char *nm = (const volatile char *)(RPM_MSG_RAM + data + at + 8);
			if (nl >= 12 && nm[0] == 'r' && nm[4] == 'r' && nm[5] == 'e' && nm[6] == 'q')
				lcid = (int)p1;
			len = 8 + ((nl + 7) & ~7u);
		} else if (cmd == GLINK_TX_DATA) {
			uint32_t chunk = mr_rd(data + at + 8);
			if (mr_rd(data + at + 16 + 16) == 0x616f646c)	/* "ldoa" */
				ldo++;
			sent++;
			len = 16 + ((chunk + 7) & ~7u);
		} else if (cmd == GLINK_CLOSE && (int)p1 == lcid) {
			lcid = -1;					/* closed again by ABL */
		} else if (cmd == GLINK_INTENT) {
			len = 8 + 8 * mr_rd(data + at + 4);		/* count x (size, iid) */
		} else if (cmd > 14) {
			break;
		}
		at += len;
	}
	con_hex2("RPM", "rpm_requests lcid=", (uint32_t)lcid, "data pkts=", sent);
	con_hex2("RPM", "of them ldoa=", ldo, "walked to=", at);
	return lcid;
}

/* Queue count words (a multiple of 2) into ap2r and ring the RPM. */
static int rpm_put(const uint32_t *w, uint32_t count)
{
	uint32_t tail = mr_rd(rpm_tx.off), head = mr_rd(rpm_tx.off + 4);
	uint32_t room = tail <= head ? rpm_tx.size - head + tail : tail - head;
	if (count * 4 + 8 > room || (head & 7) || head >= rpm_tx.size)
		return -1;
	for (uint32_t i = 0; i < count; i++) {
		mr_wr(rpm_tx.off + 8 + head, w[i]);
		head = (head + 4) % rpm_tx.size;
	}
	__asm__ volatile("dsb sy" ::: "memory");
	mr_wr(rpm_tx.off + 4, head);
	__asm__ volatile("dsb sy" ::: "memory");
	*(volatile uint32_t *)RPM_IPC_DOORBELL = 1;
	return 0;
}

static uint32_t rx_word(uint32_t at) { return mr_rd(rpm_rx.off + 8 + at % rpm_rx.size); }

/*
 * Next packet from the RPM within timeout: its first two words (and for an
 * OPEN whether it names rpm_requests); the packet is consumed. -1: none.
 */
static int rpm_get(uint32_t *w0, uint32_t *w1, int *is_req, uint64_t timeout)
{
	uint64_t end = ticks() + timeout;
	uint32_t tail = mr_rd(rpm_rx.off);
	while (mr_rd(rpm_rx.off + 4) == tail)
		if (ticks() > end)
			return -1;
	*w0 = rx_word(tail);
	*w1 = rx_word(tail + 4);
	uint32_t cmd = *w0 & 0xffff, len = 8;
	*is_req = 0;
	if (cmd == GLINK_OPEN) {
		*is_req = (rx_word(tail + 8) & 0xffffffff) == 0x5f6d7072;	/* "rpm_" */
		len = 8 + (((*w1 & 0xffff) + 7) & ~7u);
	} else if (cmd == GLINK_TX_DATA) {
		len = 16 + ((rx_word(tail + 8) + 7) & ~7u);
	} else if (cmd == GLINK_INTENT) {
		len = 8 + 8 * *w1;
	}
	mr_wr(rpm_rx.off, (tail + len) % rpm_rx.size);
	return 0;
}

/*
 * Nobody talked to the RPM yet (both FIFOs at 0): what Linux's glink-rpm does
 * at probe. VERSION 1 without features both ways, then OPEN "rpm_requests" as
 * our channel 1 and OPEN_ACK of the RPM's own OPEN. Returns 1 (our lcid).
 */
static int rpm_open(void)
{
	static const uint32_t ver[2] = { 0 /* VERSION */ | 1u << 16, 0 };
	static const uint32_t open[6] = { GLINK_OPEN | 1u << 16, 13,
		0x5f6d7072, 0x75716572, 0x73747365, 0 };		/* "rpm_requests\0" */
	uint32_t w0, w1, rcid = 0, got = 0;
	int is_req;
	mr_wr(rpm_rx.off, mr_rd(rpm_rx.off + 4));		/* start from what is there */
	if (rpm_put(ver, 2))
		return -1;
	uint64_t end = ticks() + TIMEOUT_TICKS;			/* 0.5 s */
	while (ticks() < end && got != 7) {
		if (rpm_get(&w0, &w1, &is_req, TIMEOUT_TICKS / 10))
			continue;
		uint32_t cmd = w0 & 0xffff, p1 = w0 >> 16;
		if (cmd == 0) {					/* the RPM's VERSION: ack it */
			uint32_t ack[2] = { 1 | p1 << 16, 0 };
			(void)rpm_put(ack, 2);
			if (!(got & 1)) {			/* negotiated either way */
				got |= 1;
				(void)rpm_put(open, 6);
			}
		} else if (cmd == 1 && !(got & 1)) {		/* VERSION_ACK: now OPEN */
			got |= 1;
			(void)rpm_put(open, 6);
		} else if (cmd == GLINK_OPEN_ACK && p1 == 1) {
			got |= 2;
		} else if (cmd == GLINK_OPEN && is_req) {
			uint32_t ack[2] = { GLINK_OPEN_ACK | p1 << 16, 0 };
			rcid = p1;
			got |= 4;
			(void)rpm_put(ack, 2);
		}
	}
	con_hex2("RPM glink", "got(ver|ack|open)=", got, "rcid=", rcid);
	return got == 7 ? 1 : -1;
}

/* Ask for ldoa id on at uv; prints the RPM's first answer words. 0 on an answer. */
static int rpm_ldo_on(int lcid, uint32_t id, uint32_t uv, uint32_t msg_id)
{
	uint32_t m[20] = {
		GLINK_TX_DATA | (uint32_t)lcid << 16, 0, 28 + 36, 0,
		0x00716572 /* "req" */, 20 + 36, msg_id, 0 /* active set */,
		0x616f646c /* "ldoa" */, id, 36,
		0x00007675 /* "uv" */, 4, uv,
		0x646d736c /* "lsmd" */, 4, 7 /* high power */,
		0x6e657773 /* "swen" */, 4, 1,
	};
	uint32_t w0, w1;
	int is_req;
	if (rpm_put(m, 20)) {
		con_hex2("RPM ldoa", "no room id=", id, "", 0);
		return -1;
	}
	/* The answer: TX_DATA on the RPM's channel, service "msg" (or "err"). */
	uint32_t tail = mr_rd(rpm_rx.off);
	for (int i = 0; i < 4; i++) {
		if (rpm_get(&w0, &w1, &is_req, TIMEOUT_TICKS / 5)) {
			con_hex2("RPM ldoa", "no answer id=", id, "uv=", uv);
			return -1;
		}
		if ((w0 & 0xffff) == GLINK_TX_DATA) {
			con_hex2("RPM ldoa", "id=", id, "svc=", rx_word(tail + 16));
			con_hex2("RPM ldoa", "w2=", rx_word(tail + 24), "w3=", rx_word(tail + 28));
			return 0;
		}
		tail = mr_rd(rpm_rx.off);
	}
	return -1;
}

/*
 * The microSD's supplies through the RPM: VDD-IO (ldoa 5) and VDD (ldoa 22)
 * at 2.952 V. ABL leaves both off and no GLINK session behind, so open
 * rpm_requests first unless an earlier call did. RPM votes outlive the shim:
 * xnu's SD backend (/sweet-sdhci sd-card) relies on them. 0 when both answered.
 */
int sd_power_on(void)
{
	int lcid = rpm_find();
	if (lcid < 0 && rpm_tx.size && mr_rd(rpm_tx.off + 4) == 0 && mr_rd(rpm_rx.off + 4) == 0)
		lcid = rpm_open();
	if (lcid < 0) {
		con_str("RPM", "no rpm_requests channel: SD supplies stay off");
		return -1;
	}
	int r = rpm_ldo_on(lcid, 5, 2952000, 0x5d05);		/* vdd-io, 3.3 V signalling */
	r |= rpm_ldo_on(lcid, 22, 2952000, 0x5d22);		/* vdd */
	delay_us(10000);
	return r;
}

void test_sdcard(void)
{
	con_clear(BLUE);
	con_str("SD", "ginkgo microSD probe v3 (no data writes)");
	con_hex2("GCC", "ahb=", gcc_rd(GCC_SDCC2_AHB_CBCR), "apps=", gcc_rd(GCC_SDCC2_APPS_CBCR));
	con_hex2("GCC", "rcg cmd=", gcc_rd(GCC_SDCC2_RCG_CMD), "cfg=", gcc_rd(GCC_SDCC2_RCG_CFG));

	/* Root clock on XO, no divider, no M/N: the host divides it below. */
	gcc_wr(GCC_SDCC2_RCG_CFG, gcc_rd(GCC_SDCC2_RCG_CFG) & ~((3u << 12) | (7u << 8) | 0x1fu));
	gcc_wr(GCC_SDCC2_RCG_CMD, gcc_rd(GCC_SDCC2_RCG_CMD) | RCG_UPDATE);
	uint64_t end = ticks() + TIMEOUT_TICKS;
	while (gcc_rd(GCC_SDCC2_RCG_CMD) & RCG_UPDATE)
		if (ticks() > end)
			break;
	if (gcc_branch_on(GCC_SDCC2_AHB_CBCR) || gcc_branch_on(GCC_SDCC2_APPS_CBCR)) {
		con_hex2("GCC", "still off ahb=", gcc_rd(GCC_SDCC2_AHB_CBCR), "apps=", gcc_rd(GCC_SDCC2_APPS_CBCR));
		con_str("SD", "clocks did not start: not touching the host");
		return;
	}
	con_hex2("GCC", "on ahb=", gcc_rd(GCC_SDCC2_AHB_CBCR), "rcg cmd=", gcc_rd(GCC_SDCC2_RCG_CMD));

	hc_base = SD_BASE;
	uint32_t ver = hc_rd(R_VERSION), present = hc_rd(R_PRESENT);
	con_hex2("HC", "ver=", ver, "present=", present);
	con_hex2("HC", "hostctl=", hc_rd(R_HOSTCTL), "clock=", hc_rd(R_CLOCK));
	con_hex2("HC", "pwrctl=", hc_rd(R_PWRCTL_STATUS), "caps=", hc_rd(R_CAPS));
	if (ver == 0 || ver == 0xffffffff) {
		con_str("SD", "registers read as 0/ff");
		return;
	}
	/* present: bit 16 card inserted, 17 state stable, 18 card-detect pin */
	con_hex2("CARD", "inserted=", (present >> 16) & 1, "cd pin=", (present >> 18) & 1);

	(void)sd_power_on();

	/* Full host reset, acknowledged to the msm core (it asks for bus off). */
	hc_wr(R_CLOCK, hc_rd(R_CLOCK) | (1u << 24));
	end = ticks() + TIMEOUT_TICKS;
	while (hc_rd(R_CLOCK) & (1u << 24))
		if (ticks() > end)
			break;
	con_hex2("RESET", "pwrctl=", pwrctl_ack(), "clock=", hc_rd(R_CLOCK));

	hc_wr(R_INT_EN, 0xffff00ff & ~0x100u);
	hc_wr(R_SIG_EN, 0);
	hc_wr(R_INT_STAT, 0xffffffff);
	hc_wr(R_CTRL2, hc_rd(R_CTRL2) & ~(1u << (16 + 3)));	/* 3.3 V signalling */
	/* Bus power on at 3.3 V: power field 0b111 << 1 | on. */
	hc_wr(R_HOSTCTL, (hc_rd(R_HOSTCTL) & ~0xff3eu) | (0x0fu << 8));
	con_hex2("POWER", "on, pwrctl=", pwrctl_ack(), "hostctl=", hc_rd(R_HOSTCTL));

	/* 19.2 MHz / (2 * 24) = 400 kHz for identification; max data timeout. */
	hc_wr(R_CLOCK, (hc_rd(R_CLOCK) & ~(0xffu << 16)) | (0x0eu << 16));
	set_clock(24);
	con_hex2("CLOCK", "400k reg=", hc_rd(R_CLOCK), "present=", hc_rd(R_PRESENT));

	uint32_t resp = 0, st = 0;
	(void)cmd(0, 0, 0, 0, 0, &resp, &st);
	delay_us(2000);
	/* CMD8: 2.7-3.6 V, check pattern 0xaa. No answer = v1 card or no supply. */
	int e = cmd(8, 0x1aa, C_RESP48 | C_CRC | C_INDEX, 0, 0, &resp, &st);
	con_hex2("CMD8", "resp=", e ? 0 : resp, "st=", st);
	if (e)
		soft_reset_cmd_dat();

	/* ACMD41 with HCS until the card leaves its busy state (up to ~1 s). */
	uint32_t ocr = 0;
	for (int i = 0; i < 1000 && !(ocr & (1u << 31)); i++) {
		if (cmd(55, 0, C_RESP48 | C_CRC | C_INDEX, 0, 0, &resp, &st) ||
		    cmd(41, 0x40ff8000, C_RESP48, 0, 0, &ocr, &st)) {
			soft_reset_cmd_dat();
			ocr = 0;
		}
		if (!(ocr & (1u << 31)))
			delay_us(1000);
	}
	con_hex2("ACMD41", "ocr=", ocr, "st=", st);
	if (!(ocr & (1u << 31))) {
		con_str("SD", "no ready OCR: card supply off (RPM L22/L5)?");
		return;
	}
	con_hex2("OCR", "ccs (SDHC/XC)=", (ocr >> 30) & 1, "3.3V window=", (ocr >> 20) & 3);
	if (cmd(2, 0, C_RESP136 | C_CRC, 0, 0, &resp, &st)) {
		con_hex2("CMD2", "failed st=", st, "", 0);
		return;
	}
	/* R2 without CRC, shifted by 8: MID is the top byte of the last word. */
	con_hex2("CID", "w3=", hc_rd(R_RESP0 + 12), "w2=", hc_rd(R_RESP0 + 8));
	if (cmd(3, 0, C_RESP48 | C_CRC | C_INDEX, 0, 0, &resp, &st)) {
		con_hex2("CMD3", "failed st=", st, "", 0);
		return;
	}
	uint32_t rca = resp >> 16;
	if (cmd(7, rca << 16, C_RESP48B | C_CRC | C_INDEX, 0, 0, &resp, &st)) {
		con_hex2("CMD7", "failed st=", st, "rca=", rca);
		return;
	}
	wait_busy();
	(void)cmd(13, rca << 16, C_RESP48 | C_CRC | C_INDEX, 0, 0, &resp, &st);
	con_hex2("SELECTED", "rca=", rca, "state=", (resp >> 9) & 0xf);

	/* Default speed is fine for reads: 19.2 MHz / 2 = 9.6 MHz. */
	set_clock(1);
	e = cmd(17, 0, C_RESP48 | C_CRC | C_INDEX | C_DATA, M_BLKCNT | M_READ, 1, &resp, &st);
	if (e) {
		con_hex2("CMD17", "lba0 e=", (uint32_t)e, "st=", st);
		soft_reset_cmd_dat();
		return;
	}
	const uint8_t *b = (const uint8_t *)buf;
	con_hex2("LBA0", "sig=", b[510] | b[511] << 8, "part1 type=", b[450]);
	con_hex2("LBA0", "part1 start=", b[454] | b[455] << 8 | b[456] << 16 | (uint32_t)b[457] << 24,
		 "w0=", buf[0]);
	con_str("SD", (b[510] == 0x55 && b[511] == 0xaa) ? "OK: microSD readable from our code"
							   : "read done, no MBR signature");
}
