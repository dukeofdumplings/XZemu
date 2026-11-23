/*
 * i.MX6 APBH channel-zero transport trace for XDJ-XZ U-Boot.
 *
 * The XDJ-XZ manufacturer's U-Boot tree configures APBH v2 channel 0 for
 * GPMI and emits an in-RAM mxs_dma_cmd descriptor.  This model implements
 * only the registers used to launch that descriptor, decodes it, and applies
 * its GPMI PIO words.  It transfers only the observed guest-memory-to-GPMI
 * command/address/data writes.  It deliberately has no NAND media, ready/
 * busy state, ID, response data, or BCH result.
 */
#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/misc/imx6_apbh_dma.h"
#include "migration/vmstate.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "system/address-spaces.h"
#include "system/memory.h"

#define APBH_CTRL0              0x000
#define APBH_CTRL0_SET          0x004
#define APBH_CTRL0_CLR          0x008
#define APBH_CTRL0_TOG          0x00c
#define APBH_CTRL1              0x010
#define APBH_CTRL1_SET          0x014
#define APBH_CTRL1_CLR          0x018
#define APBH_CTRL1_TOG          0x01c
#define APBH_CTRL2              0x020
#define APBH_CTRL2_SET          0x024
#define APBH_CTRL2_CLR          0x028
#define APBH_CTRL2_TOG          0x02c
#define APBH_CHANNEL_CTRL       0x030
#define APBH_CHANNEL_CTRL_SET   0x034
#define APBH_CHANNEL_CTRL_CLR   0x038
#define APBH_CHANNEL_CTRL_TOG   0x03c
#define APBH_VERSION            0x800

#define APBH_CH0_CURCMDAR       0x100
#define APBH_CH0_NXTCMDAR       0x110
#define APBH_CH0_CMD            0x120
#define APBH_CH0_SEMA           0x140

#define APBH_SFTRST             (1U << 31)
#define APBH_CLKGATE            (1U << 30)
#define APBH_CH0_GATE           (1U << 0)
#define APBH_VERSION_MAJOR      3U

#define APBH_CMD_COMMAND_MASK   0x3
#define APBH_CMD_CHAIN          (1U << 2)
#define APBH_CMD_IRQ            (1U << 3)
#define APBH_CMD_NANDWAITREADY  (1U << 5)
#define APBH_CMD_DEC_SEM        (1U << 6)
#define APBH_CMD_WAIT4END       (1U << 7)
#define APBH_CMD_PIO_WORDS_MASK 0x0000f000
#define APBH_CMD_PIO_WORDS_SHIFT 12
#define APBH_CMD_BYTES_MASK     0xffff0000
#define APBH_CMD_BYTES_SHIFT    16

#define APBH_CMD_NO_DMA_XFER    0
#define APBH_CMD_DMA_WRITE      1
#define APBH_CMD_DMA_READ       2
#define APBH_CMD_DMA_SENSE      3

#define APBH_PENDING_NONE       0
#define APBH_PENDING_LINUX_CMD  1

#define GPMI_BASE               0x00112000
#define GPMI_CTRL0              0x000
#define GPMI_COMPARE            0x010
#define GPMI_ECCCTRL            0x020
#define GPMI_ECCCOUNT           0x030
#define GPMI_PAYLOAD            0x040
#define GPMI_AUXILIARY          0x050
#define GPMI_COMMAND_MODE_SHIFT 24
#define GPMI_COMMAND_MODE_MASK  0x03000000
#define GPMI_CS_SHIFT           20
#define GPMI_CS_MASK            0x00700000
#define GPMI_ADDRESS_SHIFT      17
#define GPMI_ADDRESS_MASK       0x000e0000
#define GPMI_ADDRESS_INCREMENT  0x00010000
#define GPMI_XFER_COUNT_MASK    0x0000ffff

/* ARM's 32-bit mxs_dma_cmd: next, command, buffer address, then PIO words. */
#define APBH_DESC_WORDS         18

static const char *apbh_command_name(uint32_t command)
{
    switch (command) {
    case APBH_CMD_NO_DMA_XFER:
        return "NO_DMA_XFER";
    case APBH_CMD_DMA_WRITE:
        return "DMA_WRITE";
    case APBH_CMD_DMA_READ:
        return "DMA_READ";
    case APBH_CMD_DMA_SENSE:
        return "DMA_SENSE";
    default:
        return "INVALID";
    }
}

static const char *gpmi_address_name(uint32_t address)
{
    switch (address) {
    case 0:
        return "NAND_DATA";
    case 1:
        return "NAND_CLE";
    case 2:
        return "NAND_ALE";
    default:
        return "RESERVED";
    }
}

static const char *gpmi_mode_name(uint32_t mode)
{
    switch (mode) {
    case 0:
        return "WRITE";
    case 1:
        return "READ";
    case 2:
        return "READ_AND_COMPARE";
    case 3:
        return "WAIT_FOR_READY";
    default:
        return "INVALID";
    }
}

static void imx6_apbh_clear_pending(IMX6APBHDMAState *s)
{
    s->pending_kind = APBH_PENDING_NONE;
    s->pending_next_descriptor = 0;
    s->pending_ctrl0 = 0;
    s->pending_xfer_count = 0;
    s->pending_cs = 0;
    s->pending_address = 0;
}

static void imx6_apbh_stop(IMX6APBHDMAState *s, const char *reason)
{
    imx6_gpmi_diagnostic_apbh_cancellation(s->gpmi);
    imx6_apbh_clear_pending(s);
    s->transport_stopped = true;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "XZ_APBH: stop ch=0 reason=%s completion_irq=%u "
                  "error_irq=0 phore=%u\n", reason,
                  !!(s->regs[APBH_CTRL1 / sizeof(uint32_t)] & 1),
                  s->ch0_phore);
}

/* Channel-completion status is level-signalled until CTRL1_CLR acknowledges it. */
static void imx6_apbh_update_irq(IMX6APBHDMAState *s)
{
    qemu_set_irq(s->irq,
                 !!(s->regs[APBH_CTRL1 / sizeof(uint32_t)] & 1));
}

static void imx6_apbh_start_ch0(IMX6APBHDMAState *s);
static void imx6_apbh_reset_ch0(IMX6APBHDMAState *s);

/*
 * Linux's mxs_reset_block() may write CTRL0 directly or via CTRL0_SET.
 * APBH hardware immediately asserts CLKGATE with SFTRST, then lets software
 * poll and clear SFTRST followed by CLKGATE.  Reset aborts in-flight work;
 * it never manufactures a DMA completion.
 */
static void imx6_apbh_assert_soft_reset(IMX6APBHDMAState *s)
{
    imx6_apbh_reset_ch0(s);
    s->regs[APBH_CTRL0 / sizeof(uint32_t)] |= APBH_SFTRST | APBH_CLKGATE;
}

static void imx6_apbh_bch_timer(void *opaque)
{
    IMX6APBHDMAState *s = opaque;

    if (!s->bch_pending || s->transport_stopped || !s->bch) {
        return;
    }
    s->bch_pending = false;
    imx6_bch_complete(s->bch, s->bch_status0);
}

static void imx6_apbh_wait_timer(void *opaque)
{
    IMX6APBHDMAState *s = opaque;

    if (s->wait_pending && !s->transport_stopped) {
        imx6_apbh_start_ch0(s);
    }
}

static void imx6_apbh_complete_ch0(IMX6APBHDMAState *s,
                                   hwaddr descriptor, uint32_t next,
                                   uint32_t command_word)
{
    bool chain = command_word & APBH_CMD_CHAIN;
    bool irq = command_word & APBH_CMD_IRQ;
    bool dec_sem = command_word & APBH_CMD_DEC_SEM;
    bool wait4end = command_word & APBH_CMD_WAIT4END;

    s->wait_pending = false;
    imx6_gpmi_diagnostic_apbh_completion(s->gpmi);

    /*
     * WAIT4END orders completion after the synchronous GPMI write above.
     * It is intentionally not a NAND ready/busy wait: this model has no
     * NAND response path.
     */
    if (dec_sem) {
        if (!s->ch0_phore) {
            imx6_apbh_stop(s, "invalid-dec-sem");
            return;
        }
        s->ch0_phore--;
    }
    if (irq) {
        /* APBH CTRL1 low bits are channel completion status bits. */
        s->regs[APBH_CTRL1 / sizeof(uint32_t)] |= 1;
        imx6_apbh_update_irq(s);
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "XZ_APBH: complete ch=0 wait4end=%u dec_sem=%u irq=%u "
                  "phore=%u completion_irq=%u chain=%u\n",
                  wait4end, dec_sem, irq, s->ch0_phore,
                  !!(s->regs[APBH_CTRL1 / sizeof(uint32_t)] & 1), chain);

    if (!chain) {
        return;
    }
    if (!next) {
        imx6_apbh_stop(s, "chain-without-next-descriptor");
        return;
    }
    if (next == descriptor) {
        imx6_apbh_stop(s, "self-referencing-descriptor-chain");
        return;
    }

    s->regs[APBH_CH0_NXTCMDAR / sizeof(uint32_t)] = next;
    imx6_apbh_start_ch0(s);
}

static bool imx6_apbh_transfer_to_gpmi(IMX6APBHDMAState *s,
                                        uint32_t buffer, uint32_t bytes,
                                        uint32_t cs, uint32_t address,
                                        bool address_increment)
{
    g_autofree uint8_t *payload = NULL;
    MemTxResult result;
    unsigned int i;

    if (!s->gpmi || cs != 0) {
        imx6_apbh_stop(s, "unsupported-nand-controller-or-chip-select");
        return false;
    }
    payload = g_malloc(bytes);
    result = address_space_read(&address_space_memory, buffer,
                                MEMTXATTRS_UNSPECIFIED, payload, bytes);
    if (result != MEMTX_OK) {
        imx6_apbh_stop(s, "guest-buffer-read-failed");
        return false;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "XZ_APBH: transfer ch=0 direction=memory-to-gpmi "
                  "buffer=0x%08x bytes=%u address=%s cs=%u address_increment=%u\n",
                  buffer, bytes, gpmi_address_name(address), cs,
                  address_increment);
    for (i = 0; i < bytes; i++) {
        uint32_t phase = address;

        if (address_increment && address == 1 && i > 0) {
            phase = 2;
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_APBH: byte ch=0 index=%u phase=%s value=0x%02x\n",
                      i, gpmi_address_name(phase), payload[i]);
    }
    for (i = 0; i < bytes; i++) {
        uint32_t phase = address;

        /* XZ's send_command() queues command then address bytes in one DMA. */
        if (address_increment && address == 1 && i > 0) {
            phase = 2;
        }
        if (!imx6_gpmi_nand_write(s->gpmi, phase, cs, payload[i])) {
            imx6_apbh_stop(s, "unsupported-nand-command-or-address");
            return false;
        }
    }
    return true;
}

static void imx6_apbh_start_ch0(IMX6APBHDMAState *s)
{
    uint32_t header[3];
    uint32_t pio[15];
    uint32_t next, command_word, buffer, command, pio_words, bytes;
    uint32_t sem = s->ch0_phore;
    uint32_t gpmi_ctrl0, mode, cs, address, xfer_count;
    hwaddr descriptor = s->regs[APBH_CH0_NXTCMDAR / sizeof(uint32_t)];
    MemTxResult result;
    unsigned int i;

    if (s->transport_stopped || !sem || !descriptor ||
        (s->regs[APBH_CTRL0 / sizeof(uint32_t)] &
         (APBH_CH0_GATE | APBH_SFTRST | APBH_CLKGATE))) {
        return;
    }

    imx6_gpmi_diagnostic_apbh_descriptor(s->gpmi);

    result = address_space_read(&address_space_memory, descriptor,
                                MEMTXATTRS_UNSPECIFIED, header,
                                sizeof(header));
    if (result != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_APBH: stop ch=0 reason=descriptor-read-failed "
                      "addr=0x%08" HWADDR_PRIx " completion_irq=0 error_irq=0 phore=%u\n",
                      descriptor, sem);
        imx6_apbh_stop(s, "descriptor-read-failed");
        return;
    }

    next = le32_to_cpu(header[0]);
    command_word = le32_to_cpu(header[1]);
    buffer = le32_to_cpu(header[2]);
    command = command_word & APBH_CMD_COMMAND_MASK;
    pio_words = (command_word & APBH_CMD_PIO_WORDS_MASK) >>
                APBH_CMD_PIO_WORDS_SHIFT;
    bytes = (command_word & APBH_CMD_BYTES_MASK) >> APBH_CMD_BYTES_SHIFT;

    s->regs[APBH_CH0_CURCMDAR / sizeof(uint32_t)] = descriptor;
    s->regs[APBH_CH0_CMD / sizeof(uint32_t)] = command_word;

    qemu_log_mask(LOG_GUEST_ERROR,
                  "XZ_APBH: descriptor ch=0 addr=0x%08" HWADDR_PRIx
                  " next=0x%08x command=%s bytes=%u buffer=0x%08x "
                  "pio_words=%u chain=%u irq=%u dec_sem=%u wait4end=%u phore=%u\n",
                  descriptor, next, apbh_command_name(command), bytes, buffer,
                  pio_words, !!(command_word & APBH_CMD_CHAIN),
                  !!(command_word & APBH_CMD_IRQ),
                  !!(command_word & APBH_CMD_DEC_SEM),
                  !!(command_word & APBH_CMD_WAIT4END), sem);

    if (s->pending_kind != APBH_PENDING_NONE &&
        descriptor != s->pending_next_descriptor) {
        imx6_apbh_stop(s, "linux-command-successor-mismatch");
        return;
    }

    if (pio_words > ARRAY_SIZE(pio)) {
        imx6_apbh_stop(s, "unsupported-pio-count");
        return;
    }

    result = address_space_read(&address_space_memory, descriptor + sizeof(header),
                                MEMTXATTRS_UNSPECIFIED, pio,
                                pio_words * sizeof(*pio));
    if (result != MEMTX_OK) {
        imx6_apbh_stop(s, "pio-read-failed");
        return;
    }

    /*
     * The GPMI PIO register map is sparse.  In particular, the six-word BCH
     * read setup reaches CTRL0, COMPARE, ECCCTRL, ECCCOUNT, PAYLOAD and
     * AUXILIARY; it must not be treated as six consecutive 32-bit registers.
     * Writing these exposes the authentic request to the GPMI
     * register block but cannot constitute a NAND response on its own.
     */
    static const hwaddr gpmi_pio_offsets[] = {
        GPMI_CTRL0, GPMI_COMPARE, GPMI_ECCCTRL, GPMI_ECCCOUNT,
        GPMI_PAYLOAD, GPMI_AUXILIARY,
    };
    for (i = 0; i < MIN(pio_words, ARRAY_SIZE(gpmi_pio_offsets)); i++) {
        uint32_t value = cpu_to_le32(le32_to_cpu(pio[i]));

        result = address_space_write(&address_space_memory,
                                     GPMI_BASE + gpmi_pio_offsets[i],
                                     MEMTXATTRS_UNSPECIFIED, &value,
                                     sizeof(value));
        if (result != MEMTX_OK) {
            imx6_apbh_stop(s, "gpmi-pio-write-failed");
            return;
        }
    }

    /*
     * This is an APBH completion descriptor, not a GPMI transaction.  It is
     * the source-observed terminal descriptor following the post-BCH ready
     * wait: no PIO words, no buffer, no chained successor, and only the
     * APBH completion/IRQ/semaphore flags.  In particular, do not invent a
     * zero-value GPMI CTRL0 for it.
     */
    if (command == APBH_CMD_NO_DMA_XFER && bytes == 0 && pio_words == 0 &&
        buffer == 0 && !(command_word & APBH_CMD_CHAIN)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_APBH: terminal-complete ch=0 descriptor=0x%08" HWADDR_PRIx
                      " phore=%u\n", descriptor, s->ch0_phore);
        imx6_apbh_complete_ch0(s, descriptor, next, command_word);
        /*
         * The BCH result is separate from APBH, but a synchronous chained
         * descriptor walk does not return to the virtual clock between the
         * BCH read and this terminal completion.  Service the already queued
         * result only after APBH completion, preserving the observed ordering
         * without requiring a fictitious NAND transfer or a clock advance.
         */
        if (s->bch_pending && s->bch) {
            s->bch_pending = false;
            timer_del(s->bch_timer);
            imx6_bch_complete(s->bch, s->bch_status0);
        }
        return;
    }

    if (!pio_words) {
        g_autofree uint8_t *response = NULL;

        /*
         * Pioneer Linux gpmi_send_command() first programmed a three-word
         * WRITE/CLE PIO CCW.  Its immediate successor is a zero-PIO APBH
         * DMA_READ (memory to GPMI), which must be authorized by that setup
         * rather than by whatever CTRL0 a previous request happened to use.
         */
        if (s->pending_kind == APBH_PENDING_LINUX_CMD) {
            if (command != APBH_CMD_DMA_READ || !bytes || !buffer ||
                bytes != s->pending_xfer_count ||
                (command_word & APBH_CMD_CHAIN) ||
                (command_word & (APBH_CMD_IRQ | APBH_CMD_DEC_SEM |
                                 APBH_CMD_WAIT4END)) !=
                    (APBH_CMD_IRQ | APBH_CMD_DEC_SEM | APBH_CMD_WAIT4END) ||
                !imx6_apbh_transfer_to_gpmi(s, buffer, bytes, s->pending_cs,
                                            s->pending_address,
                                            s->pending_ctrl0 &
                                            GPMI_ADDRESS_INCREMENT)) {
                imx6_apbh_stop(s, "unsupported-linux-command-terminal");
                return;
            }
            qemu_log_mask(LOG_GUEST_ERROR,
                          "XZ_APBH: linux-command-transfer ch=0 descriptor=0x%08" HWADDR_PRIx
                          " buffer=0x%08x bytes=%u ctrl0=0x%08x\\n",
                          descriptor, buffer, bytes, s->pending_ctrl0);
            imx6_apbh_clear_pending(s);
            imx6_apbh_complete_ch0(s, descriptor, next, command_word);
            return;
        }

        /*
         * This is the terminal DMA_DEV_TO_MEM half of the exact Linux
         * two-CCW read.  It deliberately has no PIO words: validation and
         * delivery use the GPMI CTRL0 state retained by its chained setup.
         */
        if (command != APBH_CMD_DMA_WRITE || !bytes || !buffer ||
            (command_word & APBH_CMD_CHAIN) ||
            (command_word & (APBH_CMD_IRQ | APBH_CMD_DEC_SEM | APBH_CMD_WAIT4END)) !=
                (APBH_CMD_IRQ | APBH_CMD_DEC_SEM | APBH_CMD_WAIT4END) ||
            !s->gpmi) {
            imx6_apbh_stop(s, "unsupported-empty-gpmi-pio");
            return;
        }
        response = g_malloc(bytes);
        if (!imx6_gpmi_nand_read(s->gpmi, 0, response, bytes)) {
            imx6_apbh_stop(s, imx6_gpmi_nand_page_media_stop_reason(s->gpmi));
            return;
        }
        result = address_space_write(&address_space_memory, buffer,
                                     MEMTXATTRS_UNSPECIFIED, response, bytes);
        if (result != MEMTX_OK) {
            imx6_apbh_stop(s, "guest-buffer-write-failed");
            return;
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_APBH: linux-read-transfer ch=0 direction=gpmi-to-memory "
                      "buffer=0x%08x bytes=%u retained-gpmi-state=1\\n",
                      buffer, bytes);
        imx6_apbh_complete_ch0(s, descriptor, next, command_word);
        return;
    }

    gpmi_ctrl0 = le32_to_cpu(pio[0]);
    mode = (gpmi_ctrl0 & GPMI_COMMAND_MODE_MASK) >> GPMI_COMMAND_MODE_SHIFT;
    cs = (gpmi_ctrl0 & GPMI_CS_MASK) >> GPMI_CS_SHIFT;
    address = (gpmi_ctrl0 & GPMI_ADDRESS_MASK) >> GPMI_ADDRESS_SHIFT;
    xfer_count = gpmi_ctrl0 & GPMI_XFER_COUNT_MASK;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "XZ_APBH: gpmi ch=0 ctrl0=0x%08x mode=%s cs=%u "
                  "address=%s xfer_count=%u pio_words=%u\n",
                  gpmi_ctrl0, gpmi_mode_name(mode), cs,
                  gpmi_address_name(address), xfer_count, pio_words);

    /*
     * The Pioneer Linux 3.0.101 gpmi_read_data() path first appends a
     * two-word, PIO-only READ setup, then appends its DMA_DEV_TO_MEM CCW.
     * mxs_dma_prep_slave_sg() chains those two CCWs: appending clears the
     * setup CCW's IRQ/DEC_SEM, while the terminal DMA_WRITE owns the callback
     * interrupt, semaphore decrement, and WAIT4END ordering.  The setup PIO
     * has updated GPMI state above; completion here must walk the chain, not
     * reject it or manufacture completion.
     */
    if (command == APBH_CMD_NO_DMA_XFER && bytes == 0 && pio_words == 2 &&
        mode == 1 && cs == 0 && address == 0 && next &&
        (command_word & APBH_CMD_CHAIN) &&
        !(command_word & (APBH_CMD_IRQ | APBH_CMD_DEC_SEM | APBH_CMD_WAIT4END)) &&
        le32_to_cpu(pio[1]) == 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_APBH: linux-read-setup ch=0 descriptor=0x%08" HWADDR_PRIx
                      " next=0x%08x ctrl0=0x%08x\\n",
                      descriptor, next, gpmi_ctrl0);
        imx6_apbh_complete_ch0(s, descriptor, next, command_word);
        return;
    }

    /*
     * gpmi_send_command() uses three PIO words to configure a WRITE/CLE
     * transfer, then appends the command-buffer DMA CCW.  Appending makes
     * this setup CCW chained and removes its completion ownership; only the
     * exact immediate successor recorded here may consume this context.
     */
    if (command == APBH_CMD_NO_DMA_XFER && bytes == 0 && pio_words == 3 &&
        mode == 0 && cs == 0 && address == 1 && xfer_count && next &&
        (gpmi_ctrl0 & GPMI_ADDRESS_INCREMENT) &&
        (command_word & APBH_CMD_CHAIN) &&
        !(command_word & (APBH_CMD_IRQ | APBH_CMD_DEC_SEM |
                          APBH_CMD_WAIT4END)) &&
        le32_to_cpu(pio[1]) == 0 && le32_to_cpu(pio[2]) == 0) {
        s->pending_kind = APBH_PENDING_LINUX_CMD;
        s->pending_next_descriptor = next;
        s->pending_ctrl0 = gpmi_ctrl0;
        s->pending_xfer_count = xfer_count;
        s->pending_cs = cs;
        s->pending_address = address;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_APBH: linux-command-setup ch=0 descriptor=0x%08" HWADDR_PRIx
                      " next=0x%08x ctrl0=0x%08x xfer_count=%u\\n",
                      descriptor, next, gpmi_ctrl0, xfer_count);
        imx6_apbh_complete_ch0(s, descriptor, next, command_word);
        return;
    }

    /*
     * The source-observed U-Boot waits have one or three PIO words.  Pioneer
     * Linux gpmi_read_page() adds the other documented shape: two words,
     * CTRL0 WAIT_FOR_READY with a zero second PIO word, immediately before
     * BCH decode.  All are control operations: APBH_CMD_NO_DMA_XFER and
     * bytes == 0 mean the count is not a DMA request.  Do not reinterpret it
     * as one or complete it before NAND's actual ready deadline.
     */
    {
        bool legacy_wait =
            (pio_words == 1 ||
             (pio_words == 3 && le32_to_cpu(pio[1]) == 0 &&
              le32_to_cpu(pio[2]) == 0)) &&
            (command_word & APBH_CMD_NANDWAITREADY);
        /* gpmi_read_page(): DMA_TRANS_NONE, flags=0, then chain to BCH. */
        bool linux_page_wait =
            pio_words == 2 && le32_to_cpu(pio[1]) == 0 && next &&
            (command_word & APBH_CMD_CHAIN) &&
            !(command_word & (APBH_CMD_NANDWAITREADY | APBH_CMD_IRQ |
                              APBH_CMD_DEC_SEM | APBH_CMD_WAIT4END));
        /*
         * The final gpmi_read_page() PIO CCW disables BCH.  It is also the
         * terminal DMA descriptor: DMA_PREP_INTERRUPT | DMA_CTRL_ACK gives
         * it IRQ, DEC_SEM and WAIT4END, with neither CHAIN nor the legacy
         * NANDWAITREADY command bit.  The observed Pioneer trace has this
         * exact three-word form and no successor descriptor.
         */
        bool linux_post_bch_wait =
            pio_words == 3 && le32_to_cpu(pio[1]) == 0 &&
            le32_to_cpu(pio[2]) == 0 && !next &&
            !(command_word & (APBH_CMD_CHAIN | APBH_CMD_NANDWAITREADY)) &&
            (command_word & (APBH_CMD_IRQ | APBH_CMD_DEC_SEM |
                             APBH_CMD_WAIT4END)) ==
                (APBH_CMD_IRQ | APBH_CMD_DEC_SEM | APBH_CMD_WAIT4END);

        if (command == APBH_CMD_NO_DMA_XFER && mode == 3 && bytes == 0 &&
            s->gpmi && cs == 0 && address == 0 &&
            (legacy_wait || linux_page_wait || linux_post_bch_wait)) {
            if (!imx6_gpmi_nand_ready(s->gpmi, cs)) {
                int64_t deadline = imx6_gpmi_nand_ready_deadline_ns(s->gpmi);

                s->wait_pending = true;
                s->wait_descriptor = descriptor;
                timer_mod_ns(s->wait_timer, deadline);
                qemu_log_mask(LOG_GUEST_ERROR,
                              "XZ_APBH: wait-ready pending ch=0 descriptor=0x%08" HWADDR_PRIx
                              " deadline-ns=%" PRId64 " phore=%u pio_words=%u "
                              "ctrl0_count=%u\n",
                              descriptor, deadline, s->ch0_phore, pio_words,
                              xfer_count);
                return;
            }
            qemu_log_mask(LOG_GUEST_ERROR,
                          "XZ_APBH: wait-ready complete ch=0 descriptor=0x%08" HWADDR_PRIx
                          " phore=%u pio_words=%u ctrl0_count=%u\n",
                          descriptor, s->ch0_phore, pio_words, xfer_count);
            imx6_apbh_complete_ch0(s, descriptor, next, command_word);
            /*
             * start_dma_with_bch_irq() waits for APBH first and BCH second. A
             * Linux post-BCH wait is the real terminal APBH CCW, so flush the
             * already pending BCH result after its APBH completion just as the
             * older zero-PIO terminal path does. Without this, the BCH timer
             * remains queued until a later virtual-clock transition even though
             * the descriptor chain has genuinely finished.
             */
            if (linux_post_bch_wait && s->bch_pending && s->bch) {
                s->bch_pending = false;
                timer_del(s->bch_timer);
                imx6_bch_complete(s->bch, s->bch_status0);
            }
            return;
        }
    }
    /*
     * DMA_TRANS_NONE CCWs have no DMA payload.  The Pioneer mxs-dma driver
     * fills their PIO words and command bits but deliberately does not write
     * ccw->xfer_bytes.  Consequently a reused six-PIO BCH CCW can retain a
     * non-zero byte count (the authentic Linux trace has one), and hardware
     * must not interpret that field as a transfer request.  The GPMI CTRL0
     * count and the six PIO words remain the complete BCH contract.
     */
    if (command == APBH_CMD_NO_DMA_XFER && pio_words == 6 && mode == 1 &&
        address == 0) {
        uint8_t logical_payload[IMX6_MACRONIX_PAGE_MAIN_SIZE];
        uint8_t logical_auxiliary[16];
        bool uncorrectable;

        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_APBH: bch-boundary ch=0 descriptor=0x%08" HWADDR_PRIx
                      " pio0=0x%08x pio1=0x%08x pio2=0x%08x pio3=0x%08x "
                      "pio4=0x%08x pio5=0x%08x transfer_length=%u\n",
                      descriptor, gpmi_ctrl0, le32_to_cpu(pio[1]),
                      le32_to_cpu(pio[2]), le32_to_cpu(pio[3]),
                      le32_to_cpu(pio[4]), le32_to_cpu(pio[5]), xfer_count);
        if (!s->bch || !imx6_bch_is_operational(s->bch)) {
            imx6_apbh_stop(s, "bch-not-operational-or-unsupported-layout");
            return;
        }
        if (cs != 0 || xfer_count != IMX6_MACRONIX_PAGE_TOTAL ||
            le32_to_cpu(pio[2]) != 0x000011ff ||
            le32_to_cpu(pio[3]) != IMX6_MACRONIX_PAGE_TOTAL ||
            !le32_to_cpu(pio[4]) || !le32_to_cpu(pio[5])) {
            imx6_apbh_stop(s, "unsupported-bch-read-contract");
            return;
        }
        /* Preserve the strict raw profile's established P9 boundary. */
        if (!imx6_gpmi_nand_has_logical_media(s->gpmi)) {
            imx6_apbh_stop(s, "unsupported-bch-ecc-decode");
            return;
        }
        if (!imx6_gpmi_nand_logical_read(s->gpmi, logical_payload,
                                          logical_auxiliary, &uncorrectable)) {
            imx6_apbh_stop(s, imx6_gpmi_nand_page_media_stop_reason(s->gpmi));
            return;
        }
        if (!uncorrectable) {
            result = address_space_write(&address_space_memory, le32_to_cpu(pio[4]),
                                         MEMTXATTRS_UNSPECIFIED, logical_payload,
                                         sizeof(logical_payload));
            if (result != MEMTX_OK) {
                imx6_apbh_stop(s, "guest-bch-payload-write-failed");
                return;
            }
        }
        result = address_space_write(&address_space_memory, le32_to_cpu(pio[5]),
                                     MEMTXATTRS_UNSPECIFIED, logical_auxiliary,
                                     sizeof(logical_auxiliary));
        if (result != MEMTX_OK) {
            imx6_apbh_stop(s, "guest-bch-auxiliary-write-failed");
            return;
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_APBH: logical-bch-read ch=0 payload=0x%08x bytes=%u "
                      "auxiliary=0x%08x bytes=%zu status=%s\n",
                      le32_to_cpu(pio[4]), IMX6_MACRONIX_PAGE_MAIN_SIZE,
                      le32_to_cpu(pio[5]), sizeof(logical_auxiliary),
                      uncorrectable ? "uncorrectable" : "decoded");
        /*
         * Record the pending result before completing the chained DMA work.
         * imx6_apbh_complete_ch0() walks that chain synchronously, and the
         * source-observed terminal descriptor will publish this result only
         * after it has completed APBH semantics.
         */
        s->bch_status0 = 0;
        s->bch_pending = true;
        timer_mod_ns(s->bch_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1);
        /* DMA completion is visible before the separate BCH completion. */
        imx6_apbh_complete_ch0(s, descriptor, next, command_word);
        return;
    }
    /*
     * Pioneer gpmi_send_page(): one six-word BCH_ENCODE PIO CCW.  The
     * controller consumes main-area payload from PIO4 and an auxiliary
     * buffer from PIO5.  The virtual-chip path stages the 2048-byte main
     * payload for the retained NAND program command; it does not claim raw
     * spare/ECC parity representation.
     */
    if (command == APBH_CMD_NO_DMA_XFER && pio_words == 6 && mode == 0 &&
        cs == 0 && address == 0 && xfer_count == 0) {
        uint32_t eccctrl = le32_to_cpu(pio[2]);
        uint32_t payload_address = le32_to_cpu(pio[4]);
        uint32_t auxiliary_address = le32_to_cpu(pio[5]);
        uint8_t payload[IMX6_MACRONIX_PAGE_MAIN_SIZE];
        uint8_t auxiliary0;

        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_APBH: bch-encode-boundary ch=0 descriptor=0x%08" HWADDR_PRIx
                      " pio0=0x%08x pio1=0x%08x pio2=0x%08x pio3=0x%08x "
                      "pio4=0x%08x pio5=0x%08x\n",
                      descriptor, gpmi_ctrl0, le32_to_cpu(pio[1]), eccctrl,
                      le32_to_cpu(pio[3]), payload_address, auxiliary_address);
        if (!s->bch || !imx6_bch_is_operational(s->bch) ||
            eccctrl != 0x000031ff || !le32_to_cpu(pio[3]) ||
            !payload_address || !auxiliary_address) {
            imx6_apbh_stop(s, "unsupported-bch-ecc-encode");
            return;
        }
        result = address_space_read(&address_space_memory, payload_address,
                                    MEMTXATTRS_UNSPECIFIED, payload,
                                    sizeof(payload));
        if (result != MEMTX_OK) {
            imx6_apbh_stop(s, "guest-bch-payload-read-failed");
            return;
        }
        /* The original GPMI driver puts the pre-swap marker byte here. */
        result = address_space_read(&address_space_memory, auxiliary_address,
                                    MEMTXATTRS_UNSPECIFIED, &auxiliary0,
                                    sizeof(auxiliary0));
        if (result != MEMTX_OK) {
            imx6_apbh_stop(s, "guest-bch-auxiliary-read-failed");
            return;
        }
        if (!imx6_gpmi_nand_stage_program(s->gpmi, payload, auxiliary0)) {
            imx6_apbh_stop(s, "nand-program-stage-rejected");
            return;
        }
        /* BCH completion retains its actual APBH-before-BCH ordering. */
        s->bch_status0 = 0;
        s->bch_pending = true;
        timer_mod_ns(s->bch_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1);
        imx6_apbh_complete_ch0(s, descriptor, next, command_word);
        /* U-Boot's page-program chain terminates at this encoder CCW and
         * immediately waits for BCH.  The transfer itself is synchronous in
         * this model, so publish the already-pending BCH completion after
         * (never before) APBH completion.  Leaving it solely on virtual time
         * turns a real completed encode into a guest-visible timeout. */
        if (s->bch_pending && s->bch) {
            s->bch_pending = false;
            timer_del(s->bch_timer);
            imx6_bch_complete(s->bch, s->bch_status0);
        }
        return;
    }
    if (pio_words > 3) {
        imx6_apbh_stop(s, "unsupported-gpmi-pio-layout");
        return;
    }

    if (command == APBH_CMD_DMA_WRITE) {
        g_autofree uint8_t *response = NULL;

        if (mode != 1 || address != 0 || xfer_count != bytes || !s->gpmi) {
            imx6_apbh_stop(s, "unsupported-gpmi-to-memory-transfer");
            return;
        }
        response = g_malloc(bytes);
        if (!imx6_gpmi_nand_read(s->gpmi, cs, response, bytes)) {
            imx6_apbh_stop(s, imx6_gpmi_nand_page_media_stop_reason(s->gpmi));
            return;
        }
        result = address_space_write(&address_space_memory, buffer,
                                     MEMTXATTRS_UNSPECIFIED, response, bytes);
        if (result != MEMTX_OK) {
            imx6_apbh_stop(s, "guest-buffer-write-failed");
            return;
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_APBH: transfer ch=0 direction=gpmi-to-memory "
                      "buffer=0x%08x bytes=%u address=NAND_DATA cs=%u\n",
                      buffer, bytes, cs);
        imx6_apbh_complete_ch0(s, descriptor, next, command_word);
        return;
    }
    if (command != APBH_CMD_DMA_READ) {
        imx6_apbh_stop(s, "unsupported-apbh-command-type");
        return;
    }
    if (mode != 0) {
        imx6_apbh_stop(s, "unsupported-gpmi-producer-mode");
        return;
    }
    if (xfer_count != bytes) {
        imx6_apbh_stop(s, "gpmi-transfer-length-mismatch");
        return;
    }
    if (!imx6_apbh_transfer_to_gpmi(s, buffer, bytes, cs, address,
                                    gpmi_ctrl0 & GPMI_ADDRESS_INCREMENT)) {
        return;
    }
    imx6_apbh_complete_ch0(s, descriptor, next, command_word);
}

static uint64_t imx6_apbh_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX6APBHDMAState *s = opaque;

    imx6_gpmi_diagnostic_access(s->gpmi, IMX6_NAND_DIAG_APBH,
                                 offset, size, false);

    if (offset == APBH_CH0_SEMA) {
        return (uint32_t)s->ch0_phore << 16;
    }
    if (offset == APBH_VERSION) {
        return APBH_VERSION_MAJOR << 24;
    }
    return s->regs[offset / sizeof(uint32_t)];
}

static void imx6_apbh_reset_ch0(IMX6APBHDMAState *s)
{
    s->regs[APBH_CH0_CURCMDAR / sizeof(uint32_t)] = 0;
    s->regs[APBH_CH0_NXTCMDAR / sizeof(uint32_t)] = 0;
    s->regs[APBH_CH0_CMD / sizeof(uint32_t)] = 0;
    s->ch0_phore = 0;
    /* This aborts an unfinished request; it does not signal completion. */
    s->transport_stopped = false;
    imx6_apbh_clear_pending(s);
    s->wait_pending = false;
    s->bch_pending = false;
    s->regs[APBH_CTRL1 / sizeof(uint32_t)] &= ~1U;
    timer_del(s->wait_timer);
    timer_del(s->bch_timer);
    imx6_apbh_update_irq(s);
}

static void imx6_apbh_apply_bits(IMX6APBHDMAState *s, hwaddr offset,
                                 uint32_t value)
{
    uint32_t index = (offset & ~0xf) / sizeof(uint32_t);

    switch (offset & 0xf) {
    case 0x0:
        s->regs[index] = value;
        break;
    case 0x4:
        s->regs[index] |= value;
        break;
    case 0x8:
        s->regs[index] &= ~value;
        break;
    case 0xc:
        s->regs[index] ^= value;
        break;
    }
}

static void imx6_apbh_write(void *opaque, hwaddr offset,
                            uint64_t value, unsigned size)
{
    IMX6APBHDMAState *s = opaque;
    uint32_t bits = value;

    imx6_gpmi_diagnostic_access(s->gpmi, IMX6_NAND_DIAG_APBH,
                                 offset, size, true);

    switch (offset) {
    case APBH_CTRL0:
    case APBH_CTRL0_SET:
    case APBH_CTRL0_CLR:
    case APBH_CTRL0_TOG:
        imx6_apbh_apply_bits(s, offset, bits);
        if (s->regs[APBH_CTRL0 / sizeof(uint32_t)] & APBH_SFTRST) {
            imx6_apbh_assert_soft_reset(s);
        }
        if (offset == APBH_CTRL0_CLR && (bits & APBH_CH0_GATE)) {
            imx6_apbh_start_ch0(s);
        }
        break;
    case APBH_CTRL1:
    case APBH_CTRL1_SET:
    case APBH_CTRL1_CLR:
    case APBH_CTRL1_TOG:
    case APBH_CTRL2:
    case APBH_CTRL2_SET:
    case APBH_CTRL2_CLR:
    case APBH_CTRL2_TOG:
    case APBH_CHANNEL_CTRL:
    case APBH_CHANNEL_CTRL_SET:
    case APBH_CHANNEL_CTRL_CLR:
    case APBH_CHANNEL_CTRL_TOG:
        imx6_apbh_apply_bits(s, offset, bits);
        if (offset >= APBH_CTRL1 && offset <= APBH_CTRL1_TOG) {
            imx6_apbh_update_irq(s);
        }
        if (offset == APBH_CHANNEL_CTRL_SET && (bits & (1U << 16))) {
            imx6_apbh_reset_ch0(s);
        }
        break;
    case APBH_CH0_NXTCMDAR:
        s->regs[offset / sizeof(uint32_t)] = bits;
        break;
    case APBH_CH0_SEMA:
        s->ch0_phore += bits & 0xff;
        imx6_apbh_start_ch0(s);
        break;
    case APBH_VERSION:
        /* Read-only source-backed major version required by mxs-dma. */
        break;
    default:
        s->regs[offset / sizeof(uint32_t)] = bits;
        break;
    }
}

static const MemoryRegionOps imx6_apbh_ops = {
    .read = imx6_apbh_read,
    .write = imx6_apbh_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = sizeof(uint32_t),
        .max_access_size = sizeof(uint32_t),
    },
};

static void imx6_apbh_reset(DeviceState *dev)
{
    IMX6APBHDMAState *s = IMX6_APBH_DMA(dev);

    memset(s->regs, 0, sizeof(s->regs));
    /* XZ U-Boot explicitly releases channel zero after installing the chain. */
    s->regs[APBH_CTRL0 / sizeof(uint32_t)] = APBH_CH0_GATE;
    s->ch0_phore = 0;
    s->transport_stopped = false;
    imx6_apbh_clear_pending(s);
    s->wait_pending = false;
    s->bch_pending = false;
    timer_del(s->wait_timer);
    timer_del(s->bch_timer);
    imx6_apbh_update_irq(s);
}

void imx6_apbh_dma_set_gpmi(IMX6APBHDMAState *s, IMX6GPMIResetState *gpmi)
{
    s->gpmi = gpmi;
}

void imx6_apbh_dma_set_bch(IMX6APBHDMAState *s, IMX6BCHState *bch)
{
    s->bch = bch;
}

static void imx6_apbh_init(Object *obj)
{
    IMX6APBHDMAState *s = IMX6_APBH_DMA(obj);

    memory_region_init_io(&s->iomem, obj, &imx6_apbh_ops, s,
                          TYPE_IMX6_APBH_DMA, IMX6_APBH_DMA_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
    s->wait_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, imx6_apbh_wait_timer, s);
    s->bch_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, imx6_apbh_bch_timer, s);
}

static int imx6_apbh_post_load(void *opaque, int version_id)
{
    IMX6APBHDMAState *s = opaque;

    imx6_apbh_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_imx6_apbh = {
    .name = TYPE_IMX6_APBH_DMA,
    .version_id = 4,
    .minimum_version_id = 1,
    .post_load = imx6_apbh_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX6APBHDMAState,
                             IMX6_APBH_DMA_SIZE / sizeof(uint32_t)),
        VMSTATE_UINT8(ch0_phore, IMX6APBHDMAState),
        VMSTATE_BOOL(transport_stopped, IMX6APBHDMAState),
        VMSTATE_BOOL_V(wait_pending, IMX6APBHDMAState, 2),
        VMSTATE_UINT32_V(wait_descriptor, IMX6APBHDMAState, 2),
        VMSTATE_TIMER_PTR_V(wait_timer, IMX6APBHDMAState, 2),
        VMSTATE_BOOL_V(bch_pending, IMX6APBHDMAState, 3),
        VMSTATE_UINT32_V(bch_status0, IMX6APBHDMAState, 3),
        VMSTATE_TIMER_PTR_V(bch_timer, IMX6APBHDMAState, 3),
        VMSTATE_UINT8_V(pending_kind, IMX6APBHDMAState, 4),
        VMSTATE_UINT32_V(pending_next_descriptor, IMX6APBHDMAState, 4),
        VMSTATE_UINT32_V(pending_ctrl0, IMX6APBHDMAState, 4),
        VMSTATE_UINT32_V(pending_xfer_count, IMX6APBHDMAState, 4),
        VMSTATE_UINT8_V(pending_cs, IMX6APBHDMAState, 4),
        VMSTATE_UINT8_V(pending_address, IMX6APBHDMAState, 4),
        VMSTATE_END_OF_LIST()
    },
};

static void imx6_apbh_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, imx6_apbh_reset);
    dc->vmsd = &vmstate_imx6_apbh;
    dc->desc = "i.MX6 APBH channel-zero GPMI transport";
}

static const TypeInfo imx6_apbh_type_info = {
    .name = TYPE_IMX6_APBH_DMA,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMX6APBHDMAState),
    .instance_init = imx6_apbh_init,
    .class_init = imx6_apbh_class_init,
};

static void imx6_apbh_register_types(void)
{
    type_register_static(&imx6_apbh_type_info);
}

type_init(imx6_apbh_register_types)
