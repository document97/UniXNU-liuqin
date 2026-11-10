/*
 * Novatek NT36532 touch for liuqin (Xiaomi Pad 6 Pro, SM8475), polled, over the
 * GENI SPI of QUP0 SE4.
 *
 * The SoC side (TLMM pins, GCC clocks, the GENI FIFO transfer and the bit-banged
 * fallback) is pong_goodix.c's (APSL 2.0): pong is the same SM8475 with its touch
 * on the same serial engine and the same pins 16-19 and 21. liuqin has no touch
 * reset line and no touch supplies of its own (the panel feeds the TDDI die), so
 * the RPMh part of that file is not needed.
 *
 * The NT36532 is flashless: after the panel came up (ABL draws the splash) the
 * chip sits in its boot loader until the host downloads the application image
 * into SRAM. Protocol facts (registers, framing, the hardware-CRC download, the
 * 65-byte point report) were taken from the Novatek driver of the mainline liuqin
 * tree (yzddmr6/linux-sm8450-liuqin, drivers/input/touchscreen/nt36523, GPL) and
 * ShengNovatekTouch (Apache-2.0, same NT36532 on the Pad 6S Pro); no code was
 * copied from either. Device quirks found on liuqin hardware by iPadOS-liuqin
 * (GENI duplex opcode 3, the CRC reboot loop and its 0xa5 escape, the status
 * handshake the firmware wants within ~18 s) are applied here.
 *
 * Samples go to /dev/pong-touch through pong_touch_post(), normalized to 0-65535,
 * and the existing backboardd bridge (userland/touch.c) turns them into HID.
 *
 * Boot arguments:
 *   liuqin_touch=0        disable (default 1 when /arm-io/liuqin-touch exists)
 *   liuqin_touch_fw=N     1 = CSOT panel firmware (default), 2 = Tianma
 *   liuqin_touch_flip=N   bit 0 mirror x, bit 1 mirror y
 *   liuqin_touch_hz=N     SPI clock (default 8000000, at most 9600000)
 *   liuqin_touch_log=N    verbose report logging for the first N reports
 */
#include <pexpert/arm64/board_config.h>
#if defined(PONG)
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/pong_input.h>
#include <kern/sched_prim.h>
#include <pexpert/device_tree.h>
#include <sys/systm.h>

#include "liuqin_nvt_fw.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);

#define TAG "liuqin touch: "

static vm_offset_t tlmm, gcc, se;
static uint64_t tlmm_phys, gcc_phys, se_phys;
static uint32_t pins[6], gpio_offset, panel_w, panel_h;

/* ---- board description (/arm-io/liuqin-touch, configs/liuqin-board.json) -------------------------- */

static bool
property(DTEntry entry, const char *name, void *out, unsigned size)
{
	void *value = NULL;
	unsigned length = 0;

	if (SecureDTGetProperty(entry, name, &value, &length) != kSuccess || length != size) {
		return false;
	}
	memcpy(out, value, size);
	return true;
}

static bool
configuration(void)
{
	DTEntry entry;

	if (SecureDTLookupEntry(NULL, "/arm-io/liuqin-touch", &entry) != kSuccess) {
		return false;
	}
	/* Only the wiring this file was written for: anything else is a different board. */
	return property(entry, "spi-base", &se_phys, 8) && property(entry, "tlmm-base", &tlmm_phys, 8) &&
	       property(entry, "gcc-base", &gcc_phys, 8) && property(entry, "gpio-offset", &gpio_offset, 4) &&
	       property(entry, "pins", pins, sizeof(pins)) && property(entry, "max-x", &panel_w, 4) &&
	       property(entry, "max-y", &panel_h, 4) && se_phys == 0x990000 && tlmm_phys == 0xf000000 &&
	       gcc_phys == 0x100000 && gpio_offset == 0x100000 && pins[0] == 16 && pins[1] == 17 &&
	       pins[2] == 18 && pins[3] == 19 && pins[4] == 21 && panel_w == 1800 && panel_h == 2880;
}

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
sleep_ms(uint32_t ms)
{
	uint64_t deadline;

	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	assert_wait_deadline((event_t)&se, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

/* ---- pins (TLMM) ---------------------------------------------------------------------------------- */

#define TLMM_SIZE               0x200000UL
#define GPIO_CFG(n)             (tlmm + gpio_offset + 0x1000UL * (n))   /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (tlmm + gpio_offset + 0x1000UL * (n) + 4)
#define PULL_NONE               0
#define PIN_MISO                pins[0]
#define PIN_MOSI                pins[1]
#define PIN_CLK                 pins[2]
#define PIN_CS                  pins[3]
#define PIN_INT                 pins[4]
#define PIN_FUNCTION_SPI        1               /* qup4 on gpio16-19, as on pong */

static void
gpio_configure(unsigned pin, unsigned function, unsigned pull, unsigned ma, bool output)
{
	uint32_t value = pull | function << 2 | (ma / 2 - 1) << 6 | (output ? 1u << 9 : 0);

	wr(GPIO_CFG(pin), (rd(GPIO_CFG(pin)) & ~0x3ffu) | value);
}

static void
gpio_set(unsigned pin, bool high)
{
	wr(GPIO_IO(pin), high ? 2 : 0);
}

static bool
gpio_get(unsigned pin)
{
	return rd(GPIO_IO(pin)) & 1;
}

static void
gpio_output(unsigned pin, bool high, unsigned pull, unsigned ma)
{
	gpio_set(pin, high);
	gpio_configure(pin, 0, pull, ma, true);
}

/* ---- clocks (GCC), as pong_goodix.c ---------------------------------------------------------------- */

#define GCC_SIZE                0x70000UL
#define GCC_VOTE                (gcc + 0x62008)
#define VOTE_QUP0_CORE_2X       (1u << 9)
#define VOTE_QUP0_CORE          (1u << 8)
#define VOTE_QUP0_M_AHB         (1u << 6)
#define VOTE_QUP0_S_AHB         (1u << 7)
#define VOTE_QUP0_SE4           (1u << 14)
#define GCC_QUP0_M_AHB_CBCR     (gcc + 0x27004)
#define GCC_QUP0_S_AHB_CBCR     (gcc + 0x27008)
#define GCC_QUP0_SE4_CBCR       (gcc + 0x274dc)
#define GCC_QUP0_SE4_CMD        (gcc + 0x274e4)
#define GCC_QUP0_SE4_CFG        (gcc + 0x274e8)
#define GCC_QUP0_SE4_DFSR       (gcc + 0x274f8)
#define GCC_QUP0_SE4_PERF_CFG(l) (gcc + 0x27500 + 4 * (l))
#define GCC_QUP0_SE4_PERF_M(l)  (gcc + 0x27540 + 4 * (l))
#define GCC_QUP0_SE4_PERF_N(l)  (gcc + 0x27580 + 4 * (l))
#define CLOCK_OFF               (1u << 31)
#define XO_HZ                   19200000

static uint32_t se_source_hz, se_clock_select, spi_hz;

static uint32_t
rcg_rate(uint32_t cfg, uint32_t m, uint32_t not_n_minus_m)
{
	static const uint32_t parent[8] = { XO_HZ, 600000000, 0, 0, 0, 0, 300000000, 0 };
	uint64_t hz = parent[cfg >> 8 & 7];
	uint32_t divider = cfg & 0x1f, mode = cfg >> 12 & 3;

	if (divider) {
		hz = hz * 2 / (divider + 1);
	}
	if (mode) {
		uint32_t n = (~not_n_minus_m & 0xffff) + (m & 0xffff);

		if (n) {
			hz = hz * (m & 0xffff) / n;
		}
	}
	return (uint32_t)hz;
}

static bool
clocks_on(uint32_t want_hz)
{
	wr(GCC_VOTE, rd(GCC_VOTE) | VOTE_QUP0_CORE_2X | VOTE_QUP0_CORE | VOTE_QUP0_M_AHB | VOTE_QUP0_S_AHB);
	if (rd(GCC_QUP0_SE4_DFSR) & 1) {
		/* preset rates: the lowest that is at least the SPI clock, else the fastest one */
		uint32_t fastest = 0, fastest_level = 0;

		for (uint32_t level = 0; level < 8; level++) {
			uint32_t hz = rcg_rate(rd(GCC_QUP0_SE4_PERF_CFG(level)), rd(GCC_QUP0_SE4_PERF_M(level)),
			    rd(GCC_QUP0_SE4_PERF_N(level)));

			if (hz > fastest) {
				fastest = hz;
				fastest_level = level;
			}
			if (hz >= want_hz && (se_source_hz == 0 || hz < se_source_hz)) {
				se_source_hz = hz;
				se_clock_select = level;
			}
		}
		if (se_source_hz == 0) {
			se_source_hz = fastest;
			se_clock_select = fastest_level;
		}
		if (se_source_hz == 0) {
			printf(TAG "no usable preset clock rate\n");
			return false;
		}
	} else {
		/* the crystal, undivided */
		wr(GCC_QUP0_SE4_CFG, (rd(GCC_QUP0_SE4_CFG) & ~0x371fu) | 1);
		wr(GCC_QUP0_SE4_CMD, rd(GCC_QUP0_SE4_CMD) | 1);
		for (int i = 0; i < 500 && (rd(GCC_QUP0_SE4_CMD) & 1); i++) {
			sleep_us(10);
		}
		if (rd(GCC_QUP0_SE4_CMD) & 1) {
			printf(TAG "the serial engine's clock does not take its configuration (%#x)\n", rd(GCC_QUP0_SE4_CMD));
			return false;
		}
		se_source_hz = XO_HZ;
		se_clock_select = 0;
	}
	wr(GCC_VOTE, rd(GCC_VOTE) | VOTE_QUP0_SE4);
	for (int i = 0; i < 1000; i++) {
		if (!((rd(GCC_QUP0_M_AHB_CBCR) | rd(GCC_QUP0_S_AHB_CBCR) | rd(GCC_QUP0_SE4_CBCR)) & CLOCK_OFF)) {
			return true;
		}
		sleep_us(10);
	}
	printf(TAG "clocks stay off: %#x %#x %#x\n", rd(GCC_QUP0_M_AHB_CBCR), rd(GCC_QUP0_S_AHB_CBCR),
	    rd(GCC_QUP0_SE4_CBCR));
	return false;
}

/* ---- SPI through the serial engine (GENI FIFO), as pong_goodix.c ------------------------------------ */

#define SE_SIZE                 0x4000UL
#define GENI_FORCE_DEFAULT      (se + 0x020)
#define GENI_OUTPUT_CTRL        (se + 0x024)
#define GENI_CGC_CTRL           (se + 0x028)
#define GENI_STATUS             (se + 0x040)
#define GENI_SER_M_CLK_CFG      (se + 0x048)
#define GENI_IF_DISABLE_RO      (se + 0x064)
#define GENI_FW_REVISION_RO     (se + 0x068)
#define GENI_CLK_SEL            (se + 0x07c)
#define SPI_CPHA                (se + 0x224)
#define SPI_LOOPBACK            (se + 0x22c)
#define SPI_CPOL                (se + 0x230)
#define SPI_DEMUX_OUTPUT_INV    (se + 0x24c)
#define SPI_DEMUX_SEL           (se + 0x250)
#define GENI_BYTE_GRAN          (se + 0x254)
#define GENI_DMA_MODE_EN        (se + 0x258)
#define SPI_TRANS_CFG           (se + 0x25c)
#define GENI_TX_PACKING_CFG0    (se + 0x260)
#define GENI_TX_PACKING_CFG1    (se + 0x264)
#define SPI_WORD_LEN            (se + 0x268)
#define SPI_TX_TRANS_LEN        (se + 0x26c)
#define SPI_RX_TRANS_LEN        (se + 0x270)
#define SPI_DELAY_COUNTERS      (se + 0x278)
#define GENI_RX_PACKING_CFG0    (se + 0x284)
#define GENI_RX_PACKING_CFG1    (se + 0x288)
#define GENI_M_CMD0             (se + 0x600)
#define GENI_M_CMD_CTRL         (se + 0x604)
#define GENI_M_IRQ_STATUS       (se + 0x610)
#define GENI_M_IRQ_EN           (se + 0x614)
#define GENI_M_IRQ_CLEAR        (se + 0x618)
#define GENI_S_IRQ_CLEAR        (se + 0x648)
#define GENI_TX_FIFO            (se + 0x700)
#define GENI_RX_FIFO            (se + 0x780)
#define GENI_TX_FIFO_STATUS     (se + 0x800)
#define GENI_RX_FIFO_STATUS     (se + 0x804)
#define GENI_TX_WATERMARK       (se + 0x80c)
#define GENI_RX_WATERMARK       (se + 0x810)
#define GENI_RX_RFR_WATERMARK   (se + 0x814)
#define SE_DMA_TX_IRQ_CLR       (se + 0xc44)
#define SE_DMA_RX_IRQ_CLR       (se + 0xd44)
#define SE_GSI_EVENT_EN         (se + 0xe18)
#define SE_IRQ_EN               (se + 0xe1c)
#define SE_HW_PARAM_0           (se + 0xe24)
#define SE_DMA_GENERAL_CFG      (se + 0xe30)

#define M_CMD_DONE              (1u << 0)
#define M_CMD_CANCEL            (1u << 4)
#define M_CMD_ABORT             (1u << 5)
#define M_ERRORS                ((1u << 1) | (1u << 2) | (1u << 3) | (1u << 24) | (1u << 25) | (1u << 28) | (1u << 29))
/* Opcode 3, Linux's TX|RX: on liuqin's SE4 opcode 7 clocks the bytes out but receives garbage (iPadOS-liuqin). */
#define SPI_FULL_DUPLEX         (3u << 27)

static uint32_t fifo_depth;

static const char *
geni_spi_init(uint32_t hz)
{
	uint32_t divider;

	if ((rd(GENI_FW_REVISION_RO) >> 8 & 0xff) != 1) {
		return "the serial engine has no SPI firmware";
	}
	if (rd(GENI_IF_DISABLE_RO) & 1) {
		return "the serial engine's FIFO interface is disabled";
	}
	fifo_depth = rd(SE_HW_PARAM_0) >> 16 & 0x3f;
	if (fifo_depth < 4) {
		return "the serial engine reports no FIFO";
	}
	wr(SE_GSI_EVENT_EN, 0);
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(GENI_S_IRQ_CLEAR, 0xffffffff);
	wr(SE_DMA_TX_IRQ_CLR, 0xffffffff);
	wr(SE_DMA_RX_IRQ_CLR, 0xffffffff);
	wr(GENI_CGC_CTRL, rd(GENI_CGC_CTRL) | 0x7f);
	wr(SE_DMA_GENERAL_CFG, rd(SE_DMA_GENERAL_CFG) | 0xf);
	wr(GENI_OUTPUT_CTRL, 0x7f);
	wr(GENI_FORCE_DEFAULT, 1);
	wr(GENI_DMA_MODE_EN, rd(GENI_DMA_MODE_EN) & ~1u);
	wr(GENI_RX_WATERMARK, fifo_depth - 3);
	wr(GENI_RX_RFR_WATERMARK, fifo_depth - 2);
	wr(GENI_TX_WATERMARK, 0);
	wr(SE_IRQ_EN, rd(SE_IRQ_EN) | 0xf);
	wr(GENI_M_IRQ_EN, rd(GENI_M_IRQ_EN) | 0x7fc0007f);

	/* mode 0, chip select 0, 8 bits, most significant first, four bytes in a FIFO word */
	wr(SPI_LOOPBACK, 0);
	wr(SPI_DEMUX_SEL, 0);
	wr(SPI_CPHA, 0);
	wr(SPI_CPOL, 0);
	wr(SPI_DEMUX_OUTPUT_INV, 0);
	wr(SPI_TRANS_CFG, rd(SPI_TRANS_CFG) & ~2u);
	wr(SPI_DELAY_COUNTERS, 0);
	wr(GENI_TX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_TX_PACKING_CFG1, 0x000ffefe);
	wr(GENI_RX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_RX_PACKING_CFG1, 0x000ffefe);
	wr(GENI_BYTE_GRAN, 0);
	wr(SPI_WORD_LEN, 8 - 4);

	divider = (se_source_hz + hz - 1) / hz;
	if (divider == 0) {
		divider = 1;
	}
	spi_hz = se_source_hz / divider;
	wr(GENI_CLK_SEL, se_clock_select & 7);
	wr(GENI_SER_M_CLK_CFG, divider << 4 | 1);
	return NULL;
}

static void
geni_drain(uint8_t *rx, uint32_t length, uint32_t *received)
{
	uint32_t status = rd(GENI_RX_FIFO_STATUS), words = status & 0x01ffffff, bytes = words * 4;

	if (words > fifo_depth) {
		return;
	}
	if (status & 0x80000000u) {
		uint32_t valid = status >> 28 & 7;

		if (valid && valid < 4) {
			bytes -= 4 - valid;
		}
	}
	for (uint32_t i = 0; i < words; i++) {
		uint32_t word = rd(GENI_RX_FIFO);

		for (int j = 0; j < 4 && bytes; j++, bytes--) {
			if (*received < length) {
				if (rx) {
					rx[*received] = (uint8_t)(word >> (8 * j));
				}
				(*received)++;
			}
		}
	}
}

static void
geni_recover(void)
{
	wr(GENI_M_CMD_CTRL, 1u << 2);
	for (int i = 0; i < 2000 && !(rd(GENI_M_IRQ_STATUS) & M_CMD_CANCEL); i++) {
		sleep_us(100);
	}
	if (!(rd(GENI_M_IRQ_STATUS) & M_CMD_CANCEL)) {
		wr(GENI_M_CMD_CTRL, 1u << 1);
		for (int i = 0; i < 2000 && !(rd(GENI_M_IRQ_STATUS) & M_CMD_ABORT); i++) {
			sleep_us(100);
		}
	}
	for (int i = 0; i < 64 && (rd(GENI_RX_FIFO_STATUS) & 0x01ffffff); i++) {
		(void)rd(GENI_RX_FIFO);
	}
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
}

/* One transfer with the chip selected throughout: `length` bytes out (zeros without tx) and as many in. */
static bool
geni_transfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	static unsigned complaints;
	uint32_t sent = 0, received = 0, status = 0;
	uint64_t deadline, budget_us;

	for (int i = 0; rd(GENI_STATUS) & 1; i++) {
		if (i > 2000) {
			geni_recover();
			break;
		}
		sleep_us(100);
	}
	/* Three times the wire time plus 20 ms: a 4 KiB firmware chunk must not hit a fixed timeout. */
	budget_us = 20000 + (uint64_t)length * 8 * 3 * 1000000 / (spi_hz ? spi_hz : 1000000);
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(SPI_TX_TRANS_LEN, length);
	wr(SPI_RX_TRANS_LEN, length);
	wr(GENI_M_CMD0, SPI_FULL_DUPLEX);
	clock_interval_to_deadline((uint32_t)budget_us, NSEC_PER_USEC, &deadline);
	while (mach_absolute_time() < deadline) {
		while (sent < length && (rd(GENI_TX_FIFO_STATUS) & 0x0fffffff) + 1 < fifo_depth) {
			uint32_t word = 0;

			for (int j = 0; j < 4 && sent < length; j++, sent++) {
				word |= (uint32_t)(tx ? tx[sent] : 0) << (8 * j);
			}
			wr(GENI_TX_FIFO, word);
		}
		status = rd(GENI_M_IRQ_STATUS);
		geni_drain(rx, length, &received);
		if (status & M_ERRORS) {
			break;
		}
		if (status & M_CMD_DONE) {
			geni_drain(rx, length, &received);
			wr(GENI_M_IRQ_CLEAR, status);
			if (received == length) {
				return true;
			}
			break;
		}
	}
	if (complaints++ < 8) {
		printf(TAG "SPI transfer of %u bytes failed: status %#x, sent %u, received %u\n", length, status, sent,
		    received);
	}
	geni_recover();
	return false;
}

/* ---- SPI by hand on the same pins (pong_goodix.c), when the engine is unusable ---------------------- */

static unsigned bitbang_in, bitbang_out;

static void
bitbang_init(void)
{
	gpio_output(PIN_CS, true, PULL_NONE, 6);
	gpio_output(PIN_CLK, false, PULL_NONE, 6);
	gpio_output(bitbang_out, false, PULL_NONE, 6);
	gpio_configure(bitbang_in, 0, PULL_NONE, 2, false);
}

static bool
bitbang_transfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	gpio_set(PIN_CS, false);
	for (uint32_t i = 0; i < length; i++) {
		uint8_t out = tx ? tx[i] : 0, in = 0;

		for (int bit = 7; bit >= 0; bit--) {
			gpio_set(bitbang_out, out >> bit & 1);
			gpio_set(PIN_CLK, true);
			in = (uint8_t)(in << 1 | gpio_get(bitbang_in));
			gpio_set(PIN_CLK, false);
		}
		if (rx) {
			rx[i] = in;
		}
	}
	gpio_set(PIN_CS, true);
	return true;
}

static bool (*spi_transfer)(const uint8_t *tx, uint8_t *rx, uint32_t length);

/* ---- NT36532 framing --------------------------------------------------------------------------------
 * Page select: FF, addr[22:15], addr[14:7]. Write: 0x80 | addr[6:0], data... Read: addr[6:0], one dummy
 * byte, then the data. The chip increments the address across pages within one transfer.
 */

#define NVT_CHUNK               4096
#define NVT_ENG_RESET           0x7fff80u
#define NVT_TRIM_ID             0x1fb104u       /* 6 bytes, [3..5] = 32 65 03 */
#define NVT_SWRST_SIF           0x1fb43eu       /* 0x69 boot loader reset, 0xaa idle */
#define NVT_CRC_ERR_FLAG        0x1fb535u
#define NVT_ENB_CASC            0x1fb12cu       /* bit 0 set: single chip */
#define NVT_BOOT_RDY            0x1fb50du
#define NVT_TX_AUTO_COPY_EN     0x1fc925u
#define NVT_ILM_LENGTH          0x1fb518u
#define NVT_ILM_DES             0x1fb528u
#define NVT_DLM_DES             0x1fb52cu
#define NVT_DLM_LENGTH          0x1fb530u
#define NVT_G_ILM_CHECKSUM      0x1fb500u
#define NVT_G_DLM_CHECKSUM      0x1fb504u
#define NVT_R_ILM_CHECKSUM      0x1fb520u
#define NVT_R_DLM_CHECKSUM      0x1fb524u
#define NVT_BLD_CRC_FLAGS       0x1fb533u
#define NVT_EVENT_BUF           0x125800u       /* same in the single and cascade maps */
#define EVENT_HOST_CMD          0x50
#define EVENT_FW_STATUS         0x51
#define EVENT_RESET_COMPLETE    0x60
#define EVENT_FWINFO            0x78
#define EVENT_PROJECT_ID        0x9a
#define RESET_STATE_INIT        0xa0
#define RESET_STATE_REK         0xa1
#define RESET_STATE_MAX         0xaf

#define NVT_FINGERS             10
#define NVT_POINT_LEN           65

static uint8_t spi_tx[NVT_CHUNK + 8], spi_rx[NVT_CHUNK + 8];
static uint32_t page_delay_us = 20;

static bool
nvt_set_page(uint32_t address)
{
	spi_tx[0] = 0xff;
	spi_tx[1] = (uint8_t)(address >> 15);
	spi_tx[2] = (uint8_t)(address >> 7);
	if (!spi_transfer(spi_tx, NULL, 3)) {
		return false;
	}
	if (page_delay_us) {
		sleep_us(page_delay_us);
	}
	return true;
}

static bool
nvt_write(uint32_t address, const uint8_t *data, uint32_t length)
{
	if (length == 0 || length > NVT_CHUNK || !nvt_set_page(address)) {
		return false;
	}
	spi_tx[0] = 0x80 | (address & 0x7f);
	memcpy(spi_tx + 1, data, length);
	return spi_transfer(spi_tx, NULL, length + 1);
}

static bool
nvt_write8(uint32_t address, uint8_t value)
{
	return nvt_write(address, &value, 1);
}

static bool
nvt_read(uint32_t address, uint8_t *data, uint32_t length)
{
	if (length == 0 || length > NVT_CHUNK || !nvt_set_page(address)) {
		return false;
	}
	memset(spi_tx, 0, length + 2);
	spi_tx[0] = address & 0x7f;
	if (!spi_transfer(spi_tx, spi_rx, length + 2)) {
		return false;
	}
	memcpy(data, spi_rx + 2, length);
	return true;
}

static uint32_t
le32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* ---- chip identity and the boot loader -------------------------------------------------------------- */

static uint8_t trim[6];
static bool cascade;

static bool
nvt_identify(bool verbose)
{
	if (!nvt_read(NVT_TRIM_ID, trim, sizeof(trim))) {
		return false;
	}
	if (trim[3] != 0x32 || trim[4] != 0x65 || trim[5] != 0x03) {
		if (verbose) {
			printf(TAG "no NT36532 at the trim address: %02x %02x %02x %02x %02x %02x\n",
			    trim[0], trim[1], trim[2], trim[3], trim[4], trim[5]);
		}
		return false;
	}
	return true;
}

static void
nvt_bootloader_reset(void)
{
	(void)nvt_write8(NVT_SWRST_SIF, 0x69);
	sleep_ms(5);
}

/*
 * A latched CRC error sends the part into a reboot loop: the trim address answers 0xfc (or nothing) and
 * the event buffer fills with 0xfd. Idle the MCU (0xaa twice to the reset register) and write 0xa5 to
 * the flag until it reads back (the vendor's nvt_stop_crc_reboot(), confirmed on liuqin).
 */
static void
nvt_stop_crc_reboot(void)
{
	uint8_t flag = 0, after = 0, id[4] = { 0 };
	bool in_loop;
	unsigned tries;

	(void)nvt_read(NVT_CRC_ERR_FLAG, &flag, 1);
	(void)nvt_read(NVT_TRIM_ID, id, sizeof(id));
	in_loop = id[0] == 0xfc || (id[0] == 0xff && id[1] == 0xff && id[2] == 0xff);
	if (flag == 0xa5 && !in_loop) {
		return;
	}
	for (tries = 0; tries < 5; tries++) {
		(void)nvt_write8(NVT_SWRST_SIF, 0xaa);
		(void)nvt_write8(NVT_SWRST_SIF, 0xaa);
		sleep_ms(1);
		(void)nvt_write8(NVT_CRC_ERR_FLAG, 0xa5);
		if (nvt_read(NVT_CRC_ERR_FLAG, &after, 1) && after == 0xa5) {
			break;
		}
	}
	printf(TAG "CRC flag %#x -> %#x after %u tries%s\n", flag, after, tries,
	    in_loop ? " (was in the CRC reboot loop)" : "");
}

/* ---- the application image ----------------------------------------------------------------------- */

#define NVT_MAX_PARTITIONS      64

struct nvt_partition {
	uint32_t bin, sram, size, crc;
};
static struct nvt_partition part[NVT_MAX_PARTITIONS];
static unsigned partitions;
static bool image_cascade_header;

/*
 * The image header (vendor layout): [0..11] ILM bin/sram/size, [12..23] DLM bin/sram/size, the ILM and
 * DLM hardware CRCs at 0x18/0x1c, flags at 0x20 (bit 1: a second, cascade header) and 0x28 (bit 4:
 * overlay sections, [3:0] their count), then 16-byte info records sram/size/bin/crc from 0x30 up to the
 * header end (the first word); overlay records sit at the start of DLM's data.
 */
static bool
image_parse(const uint8_t *fw, uint32_t size)
{
	uint32_t end, pos, info = 0, overlay = 0;
	bool found_header = false;

	if (size < 0x40) {
		return false;
	}
	end = le32(fw);
	if (end < 0x30 || end > size) {
		return false;
	}
	image_cascade_header = (fw[0x20] & 0x02) != 0;
	if (image_cascade_header) {
		for (pos = 0x30; pos < end / 2; pos += 0x10) {
			info++;
		}
		info++;
	} else {
		for (pos = 0x30; pos < end; pos += 0x10) {
			info++;
		}
	}
	if (fw[0x28] & 0x10) {
		overlay = fw[0x28] & 0x0f;
	}
	partitions = 2 + info + overlay;
	if (partitions > NVT_MAX_PARTITIONS) {
		return false;
	}
	for (unsigned i = 0; i < partitions; i++) {
		struct nvt_partition *p = &part[i];

		if (i < 2) {
			p->bin = le32(fw + 12 * i);
			p->sram = le32(fw + 12 * i + 4);
			p->size = le32(fw + 12 * i + 8);
			p->crc = le32(fw + 0x18 + 4 * i);
		} else {
			if (i < 2 + info) {
				if (!found_header) {
					pos = 0x30 + 0x10 * (i - 2);
				} else if (image_cascade_header) {
					pos = end - 0x10;
				}
			} else {
				pos = part[1].bin + 0x10 * (i - 2 - info);
			}
			if ((uint64_t)pos + 16 > size) {
				return false;
			}
			p->sram = le32(fw + pos);
			p->size = le32(fw + pos + 4);
			p->bin = le32(fw + pos + 8);
			p->crc = le32(fw + pos + 12);
			if (i < 2 + info && p->bin < end && p->size != 0) {
				found_header = true;
			}
		}
		if ((uint64_t)p->bin + p->size > size) {
			return false;
		}
	}
	return part[0].size != 0 && part[1].size != 0;
}

static bool
write3(uint32_t address, uint32_t value)
{
	const uint8_t b[3] = { (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16) };

	return nvt_write(address, b, 3);
}

static bool
write4(uint32_t address, uint32_t value)
{
	const uint8_t b[4] = { (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24) };

	return nvt_write(address, b, 4);
}

static bool
image_write(const uint8_t *fw, uint32_t size)
{
	for (unsigned i = 0; i < partitions; i++) {
		uint32_t left = part[i].size, sram = part[i].sram, bin = part[i].bin;

		if (left == 0) {
			continue;
		}
		/* the vendor flow writes one byte more than the partition when the image has it */
		if ((uint64_t)bin + left + 1 <= size) {
			left++;
		}
		while (left) {
			uint32_t n = left < NVT_CHUNK ? left : NVT_CHUNK;

			if (!nvt_write(sram, fw + bin, n)) {
				return false;
			}
			sram += n;
			bin += n;
			left -= n;
		}
	}
	return true;
}

static bool
wait_reset_state(uint8_t want, unsigned polls, uint8_t *seen)
{
	for (unsigned i = 0; i <= polls; i++) {
		uint8_t b[5] = { 0 };

		if (nvt_read(NVT_EVENT_BUF + EVENT_RESET_COMPLETE, b, sizeof(b))) {
			*seen = b[0];
			if (b[0] >= want && b[0] <= RESET_STATE_MAX) {
				return true;
			}
		}
		sleep_ms(10);
	}
	return false;
}

static void
crc_report(void)
{
	uint8_t flags = 0, b[4];
	uint32_t golden[2] = { 0 }, result[2] = { 0 };

	(void)nvt_read(NVT_BLD_CRC_FLAGS, &flags, 1);
	if (nvt_read(NVT_G_ILM_CHECKSUM, b, 4)) {
		golden[0] = le32(b);
	}
	if (nvt_read(NVT_G_DLM_CHECKSUM, b, 4)) {
		golden[1] = le32(b);
	}
	if (nvt_read(NVT_R_ILM_CHECKSUM, b, 4)) {
		result[0] = le32(b);
	}
	if (nvt_read(NVT_R_DLM_CHECKSUM, b, 4)) {
		result[1] = le32(b);
	}
	printf(TAG "boot loader CRC flags %#x, ILM %#x/%#x (image %#x), DLM %#x/%#x (image %#x)\n", flags,
	    golden[0], result[0], part[0].crc, golden[1], result[1], part[1].crc);
}

/* Hardware-CRC host download (the vendor's nvt_download_firmware_hw_crc). */
static bool
image_download(const uint8_t *fw, uint32_t size)
{
	static const uint8_t clear_reset[6] = { 0 };
	static const uint8_t crc_cmd[2] = { 0xae, 0x00 };
	uint8_t state = 0;
	uint64_t start = mach_absolute_time(), ns;

	for (unsigned attempt = 1; attempt <= 3; attempt++) {
		const char *stage;

		nvt_stop_crc_reboot();
		nvt_bootloader_reset();
		stage = "CRC bank";
		if (!write3(NVT_ILM_DES, part[0].sram) || !write3(NVT_ILM_LENGTH, part[0].size) ||
		    !write4(NVT_G_ILM_CHECKSUM, part[0].crc) || !write3(NVT_DLM_DES, part[1].sram) ||
		    !write3(NVT_DLM_LENGTH, part[1].size) || !write4(NVT_G_DLM_CHECKSUM, part[1].crc)) {
			goto failed;
		}
		if (image_cascade_header) {
			stage = "auto copy";
			if (!nvt_write8(NVT_TX_AUTO_COPY_EN, 0x56)) {
				goto failed;
			}
		}
		stage = "image";
		if (!image_write(fw, size)) {
			goto failed;
		}
		if (image_cascade_header) {
			uint8_t busy = 0xff;

			stage = "auto copy wait";
			for (unsigned i = 0; i < 200 && busy; i++) {
				if (!nvt_read(NVT_TX_AUTO_COPY_EN, &busy, 1)) {
					goto failed;
				}
				if (busy) {
					sleep_ms(1);
				}
			}
			if (busy) {
				goto failed;
			}
		}
		/* clear the reset-status window, enable the firmware CRC check, then boot */
		stage = "boot";
		if (!nvt_write(NVT_EVENT_BUF + EVENT_RESET_COMPLETE, clear_reset, sizeof(clear_reset)) ||
		    !nvt_write(NVT_EVENT_BUF + EVENT_HOST_CMD, crc_cmd, sizeof(crc_cmd)) ||
		    !nvt_write8(NVT_BOOT_RDY, 1)) {
			goto failed;
		}
		sleep_ms(5);
		stage = "reset state";
		if (!wait_reset_state(RESET_STATE_INIT, 10, &state)) {
			goto failed;
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
		printf(TAG "firmware running (state %#x) after attempt %u, %llu ms\n", state, attempt,
		    ns / NSEC_PER_MSEC);
		return true;
failed:
		printf(TAG "download attempt %u stopped at %s (state %#x)\n", attempt, stage, state);
	}
	crc_report();
	return false;
}

/* Firmware info (event buffer + 0x78), then the status handshake the firmware waits for. */
static uint32_t span_x = 18000, span_y = 28800;

static void
nvt_after_boot(void)
{
	uint8_t info[38] = { 0 }, status = 0xff, state = 0;
	const uint8_t zero = 0;

	if (!wait_reset_state(RESET_STATE_REK, 50, &state)) {
		printf(TAG "firmware did not reach ReK (state %#x)\n", state);
	}
	for (int attempt = 0; attempt < 4; attempt++) {
		if (nvt_read(NVT_EVENT_BUF + EVENT_FWINFO, info, sizeof(info)) && (uint8_t)(info[0] + info[1]) == 0xff) {
			break;
		}
		info[0] = info[1] = 0;
		sleep_ms(10);
	}
	if ((uint8_t)(info[0] + info[1]) == 0xff) {
		uint32_t x = (uint32_t)info[4] << 8 | info[5], y = (uint32_t)info[6] << 8 | info[7];

		printf(TAG "firmware %#x type %#x, sensors %u x %u, coordinates %u x %u\n", info[0], info[13],
		    info[2], info[3], x, y);
		/* Android reports 0-17999 x 0-28799 on this panel; take the firmware's span only when it agrees. */
		if (x >= 17000 && x <= 19000 && y >= 27000 && y <= 31000) {
			span_x = x;
			span_y = y;
		}
	} else {
		printf(TAG "firmware info not valid; coordinates %u x %u assumed\n", span_x, span_y);
	}
	/* nvt_clear_fw_status(): without it the part falls into its recovery loop ~18 s after boot */
	for (int i = 0; i < 20; i++) {
		if (nvt_write(NVT_EVENT_BUF + EVENT_FW_STATUS, &zero, 1) &&
		    nvt_read(NVT_EVENT_BUF + EVENT_FW_STATUS, &status, 1) && status == 0) {
			break;
		}
		sleep_ms(10);
	}
	if (status != 0) {
		printf(TAG "firmware status not cleared (%#x)\n", status);
	}
}

/* ---- reports --------------------------------------------------------------------------------------- */

static uint32_t flip, verbose_reports;
static struct {
	bool down, seen;
	uint32_t x, y;
} contact[NVT_FINGERS];
static struct {
	uint32_t polls, reads, failed, touches, recoveries;
} seen;

static void
report(const uint8_t *b)
{
	for (int i = 0; i < NVT_FINGERS; i++) {
		contact[i].seen = false;
	}
	for (int i = 0; i < NVT_FINGERS; i++) {
		const uint8_t *t = b + 6 * i;
		uint32_t id = t[0] >> 3, status = t[0] & 7, x, y;

		if (id == 0 || id > NVT_FINGERS || (status != 1 && status != 2)) {
			continue;
		}
		x = (uint32_t)t[1] << 8 | t[2];
		y = (uint32_t)t[3] << 8 | t[4];
		if (x >= span_x || y >= span_y) {
			continue;
		}
		if (verbose_reports) {
			verbose_reports--;
			printf(TAG "finger %u at %u, %u (of %u x %u), width %u\n", id - 1, x, y, span_x, span_y, t[5]);
		}
		seen.touches++;
		x = (uint32_t)((uint64_t)x * 65535 / (span_x - 1));
		y = (uint32_t)((uint64_t)y * 65535 / (span_y - 1));
		if (flip & 1) {
			x = 65535 - x;
		}
		if (flip & 2) {
			y = 65535 - y;
		}
		id--;
		contact[id].seen = true;
		if (!contact[id].down) {
			pong_touch_post(PONG_TOUCH_DOWN, id, x, y);
		} else if (contact[id].x != x || contact[id].y != y) {
			pong_touch_post(PONG_TOUCH_MOVE, id, x, y);
		}
		contact[id].down = true;
		contact[id].x = x;
		contact[id].y = y;
	}
	/* A finger missing from the report has lifted; an empty report (all 0xff, 0xfd) lifts all. */
	for (uint32_t id = 0; id < NVT_FINGERS; id++) {
		if (contact[id].down && !contact[id].seen) {
			contact[id].down = false;
			pong_touch_post(PONG_TOUCH_UP, id, contact[id].x, contact[id].y);
		}
	}
}

static void
lift_all(void)
{
	for (uint32_t id = 0; id < NVT_FINGERS; id++) {
		if (contact[id].down) {
			contact[id].down = false;
			pong_touch_post(PONG_TOUCH_UP, id, contact[id].x, contact[id].y);
		}
	}
}

/* ---- bring-up -------------------------------------------------------------------------------------- */

static const uint8_t *firmware;
static uint32_t firmware_size;
static const char *firmware_name;

static bool
bus_up(uint32_t hz)
{
	gpio_configure(PIN_INT, 0, PULL_NONE, 2, false);
	if (clocks_on(hz)) {
		const char *problem = geni_spi_init(hz);

		if (!problem) {
			gpio_configure(PIN_MISO, PIN_FUNCTION_SPI, PULL_NONE, 6, false);
			gpio_configure(PIN_MOSI, PIN_FUNCTION_SPI, PULL_NONE, 6, false);
			gpio_configure(PIN_CLK, PIN_FUNCTION_SPI, PULL_NONE, 6, false);
			gpio_configure(PIN_CS, PIN_FUNCTION_SPI, PULL_NONE, 6, false);
			spi_transfer = geni_transfer;
			if (nvt_identify(true)) {
				printf(TAG "NT36532 on GENI SPI at %u Hz (source %u Hz, FIFO %u words)\n", spi_hz,
				    se_source_hz, fifo_depth);
				return true;
			}
			/* the boot loader may need a reset before it answers */
			nvt_bootloader_reset();
			if (nvt_identify(true)) {
				return true;
			}
		} else {
			printf(TAG "GENI unavailable: %s; trying GPIO SPI\n", problem);
		}
	}
	spi_transfer = bitbang_transfer;
	for (unsigned attempt = 0; attempt < 2; attempt++) {
		bitbang_in = pins[attempt];
		bitbang_out = pins[1 - attempt];
		bitbang_init();
		if (nvt_identify(attempt == 1)) {
			printf(TAG "NT36532 on GPIO SPI (in gpio%u, out gpio%u)\n", bitbang_in, bitbang_out);
			return true;
		}
	}
	return false;
}

static void
touch_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t hz = 8000000, period_ms = 8;
	uint8_t point[NVT_POINT_LEN];
	uint64_t last_summary = 0;

	(void)PE_parse_boot_argn("liuqin_touch_hz", &hz, sizeof(hz));
	(void)PE_parse_boot_argn("liuqin_touch_page_us", &page_delay_us, sizeof(page_delay_us));
	(void)PE_parse_boot_argn("liuqin_touch_period_ms", &period_ms, sizeof(period_ms));
	if (hz < 100000 || hz > 9600000) {
		hz = 8000000;
	}
	if (period_ms < 4 || period_ms > 100) {
		period_ms = 8;
	}
	tlmm = ml_io_map(tlmm_phys, TLMM_SIZE);
	gcc = ml_io_map(gcc_phys, GCC_SIZE);
	se = ml_io_map(se_phys, SE_SIZE);
	if (!tlmm || !gcc || !se || !bus_up(hz)) {
		printf(TAG "no NT36532 answers on SPI4; normal UI boot continues without touch\n");
		thread_terminate(current_thread());
		return;
	}
	{
		uint8_t casc = 0;

		(void)nvt_read(NVT_ENB_CASC, &casc, 1);
		cascade = (casc & 1) == 0;
		printf(TAG "trim %02x %02x %02x %02x %02x %02x, %s chip\n", trim[0], trim[1], trim[2], trim[3],
		    trim[4], trim[5], cascade ? "cascade" : "single");
	}
	if (firmware_size == 0) {
		printf(TAG "this kernel carries no NT36532 firmware (%s); the chip cannot scan\n", firmware_name);
		thread_terminate(current_thread());
		return;
	}
	if (!image_parse(firmware, firmware_size)) {
		printf(TAG "%s (%u bytes) is not an NT36532 image\n", firmware_name, firmware_size);
		thread_terminate(current_thread());
		return;
	}
	printf(TAG "%s: %u bytes, %u partitions, ILM %u at %#x, DLM %u at %#x%s\n", firmware_name, firmware_size,
	    partitions, part[0].size, part[0].sram, part[1].size, part[1].sram,
	    image_cascade_header ? ", cascade header" : "");
	if (!image_download(firmware, firmware_size)) {
		printf(TAG "firmware download failed; no touch\n");
		thread_terminate(current_thread());
		return;
	}
	nvt_after_boot();
	if (!pong_touch_device_ready()) {
		printf(TAG "/dev/pong-touch could not be created\n");
		thread_terminate(current_thread());
		return;
	}
	printf(TAG "ready: %s, polling every %u ms, span %u x %u, flip %u\n", firmware_name, period_ms, span_x, span_y,
	    flip);
	for (;;) {
		uint64_t now = mach_absolute_time(), ns;

		seen.polls++;
		if (nvt_read(NVT_EVENT_BUF, point, sizeof(point))) {
			bool esd = true;

			seen.reads++;
			for (int i = 0; i < 6; i++) {
				esd &= point[i] == 0x77;
			}
			if (esd) {
				/* the firmware's ESD/watchdog signature: it lost its image; download again */
				seen.recoveries++;
				lift_all();
				printf(TAG "firmware asks for recovery; downloading again\n");
				if (image_download(firmware, firmware_size)) {
					nvt_after_boot();
				}
			} else {
				report(point);
			}
		} else {
			seen.failed++;
		}
		absolutetime_to_nanoseconds(now - last_summary, &ns);
		if (ns >= 60ull * NSEC_PER_SEC) {
			last_summary = now;
			printf(TAG "polls %u, reads %u, failed %u, touches %u, recoveries %u\n", seen.polls, seen.reads,
			    seen.failed, seen.touches, seen.recoveries);
		}
		sleep_ms(period_ms);
	}
}

void liuqin_nvt_init(void);

void
liuqin_nvt_init(void)
{
	uint32_t enabled = 1, which = 1;
	thread_t thread;

	(void)PE_parse_boot_argn("liuqin_touch", &enabled, sizeof(enabled));
	if (!enabled) {
		return;
	}
	if (!configuration()) {
		/* pong and every other board: no /arm-io/liuqin-touch, nothing to do */
		return;
	}
	(void)PE_parse_boot_argn("liuqin_touch_fw", &which, sizeof(which));
	(void)PE_parse_boot_argn("liuqin_touch_flip", &flip, sizeof(flip));
	(void)PE_parse_boot_argn("liuqin_touch_log", &verbose_reports, sizeof(verbose_reports));
	if (which == 2) {
		firmware = liuqin_nvt_fw_tm;
		firmware_size = liuqin_nvt_fw_tm_size;
		firmware_name = "novatek_nt36532_m81_fw_tm.bin";
	} else {
		firmware = liuqin_nvt_fw_csot;
		firmware_size = liuqin_nvt_fw_csot_size;
		firmware_name = "novatek_nt36532_m81_fw_csot.bin";
	}
	if (kernel_thread_start(touch_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
#endif /* PONG */
