/*
 * Source-bounded partial functional IPUv3 BG23 diagnostic stub for XDJ-XZ.
 *
 * The Pioneer Linux driver uses MEM_BG_SYNC / IDMAC channel 23.  Its exact
 * source establishes common IPU_CONF, interrupt control/status, ready-buffer
 * and IDMAC enable accesses.
 *
 * IMPLEMENTED: a bounded guest-visible register/state window, constrained
 * 800x480 RGB565 BG23 CPMEM decoding, and an opt-in functional EOF23
 * approximation. The latter consumes an eligible ready buffer and routes an
 * IRQ only through the observed control/status contract.
 *
 * NOT IMPLEMENTED: a complete IPUv3 controller, DMA, DMFC/DC/DI timing,
 * arbitrary channels or CPMEM formats, DP composition, foreground channel
 * 27, or Vivante/GPU rendering. EOF timing is a 60 Hz virtual-time
 * hypothesis, separate from the scanout stub's 10 Hz sampled refresh.
 */
#include "qemu/osdep.h"
#include "hw/misc/imx6_ipu1_trace.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "target/arm/cpu.h"

/* Pioneer ipu_common.c adds IPUV3H_REG_BASE for platform-data revision 4. */
#define IPUV3H_REG_BASE           0x200000
#define IPU_CPMEM_REG_BASE         (IPUV3H_REG_BASE + 0x100000)
#define IPU_CONF                  (IPUV3H_REG_BASE + 0x0000)
#define IPU_SRM_PRI2              (IPUV3H_REG_BASE + 0x00a4)
#define IPU_DISP_GEN              (IPUV3H_REG_BASE + 0x00c4)
#define IPU_INT_CTRL_1            (IPUV3H_REG_BASE + 0x003c)
#define IPU_INT_STAT_1            (IPUV3H_REG_BASE + 0x0200)
#define IPU_CHA_DB_MODE_SEL_23    (IPUV3H_REG_BASE + 0x0150)
#define IPU_CHA_TRB_MODE_SEL_23   (IPUV3H_REG_BASE + 0x0178)
#define IPU_CHA_TRIPLE_CUR_BUF_23 (IPUV3H_REG_BASE + 0x0258)
#define IPU_CHA_CUR_BUF_23        (IPUV3H_REG_BASE + 0x023c)
#define IPU_CHA_BUF0_RDY_23       (IPUV3H_REG_BASE + 0x0268)
#define IPU_CHA_BUF1_RDY_23       (IPUV3H_REG_BASE + 0x0270)
#define IPU_CHA_BUF2_RDY_23       (IPUV3H_REG_BASE + 0x0288)
#define IPU_GPR                   (IPUV3H_REG_BASE + 0x00e4)
#define IPU_IDMAC_CHA_EN_23       (IPUV3H_REG_BASE + 0x8004)
#define IPU_DI0_GENERAL           (IPUV3H_REG_BASE + 0x40000 + 0x0000)
#define IPU_DI0_STAT              (IPUV3H_REG_BASE + 0x40000 + 0x0174)
#define IPU_DC_WR_CH_CONF_5       (IPUV3H_REG_BASE + 0x58000 + 0x005c)
#define IPU_DC_GEN                (IPUV3H_REG_BASE + 0x58000 + 0x00d4)
#define IPU_DC_STAT               (IPUV3H_REG_BASE + 0x58000 + 0x01c8)
#define IPU_DMFC_FIRST            (IPUV3H_REG_BASE + 0x60000)
#define IPU_DMFC_LAST             (IPU_DMFC_FIRST + 0x0024)
#define IPU_BG_SYNC_BIT           (UINT32_C(1) << 23)
#define IPU_BG_SYNC_CONF_MASK     (UINT32_C(0x20) | UINT32_C(0x40) | \
                                   UINT32_C(0x200) | UINT32_C(0x400))
#define IPU_READY_CLEAR_GPR       UINT32_C(0xf0300000)
#define IPU_TRACE_MAX_EVENTS      8192
/* B1 timing hypothesis: one 60 Hz physical-panel frame in virtual time. */
#define IPU_BG23_FRAME_NS         (NANOSECONDS_PER_SECOND / 60)
#define IPU_BG23_CPMEM_CHANNEL     23
#define IPU_BG23_CPMEM_ENTRY_SIZE  64
#define IPU_BG23_CPMEM_WORD1       8
#define IPU_BG23_CPMEM_RGB565_BPP  3
#define IPU_BG23_CPMEM_RGB565_FMT  7
#define IPU_BG23_SCANOUT_WIDTH     800
#define IPU_BG23_SCANOUT_HEIGHT    480
#define IPU_BG23_SCANOUT_STRIDE    1600

enum {
    IPU_BG23_STAGE_NONE,
    IPU_BG23_STAGE_BOUNDARY,
    IPU_BG23_STAGE_EOF,
};

static const uint8_t imx6_ipu1_sync_banks[] = {
    1, 2, 3, 4, 11, 12, 13, 14, 15,
};

static uint32_t imx6_ipu1_trace_pc(void)
{
    if (current_cpu && object_dynamic_cast(OBJECT(current_cpu), TYPE_ARM_CPU)) {
        return ARM_CPU(current_cpu)->env.regs[15];
    }
    return 0;
}

static uint32_t imx6_ipu1_trace_word(const IMX6IPU1TraceState *s,
                                     hwaddr offset)
{
    return s->words[offset / sizeof(uint32_t)];
}

static bool imx6_ipu1_bg23_scan_active(const IMX6IPU1TraceState *s)
{
    uint32_t conf = imx6_ipu1_trace_word(s, IPU_CONF);
    uint32_t dc5 = imx6_ipu1_trace_word(s, IPU_DC_WR_CH_CONF_5);

    return (conf & IPU_BG_SYNC_CONF_MASK) == IPU_BG_SYNC_CONF_MASK &&
           (imx6_ipu1_trace_word(s, IPU_IDMAC_CHA_EN_23) &
            IPU_BG_SYNC_BIT) != 0 &&
           (imx6_ipu1_trace_word(s, IPU_SRM_PRI2) & UINT32_C(0x8)) != 0 &&
           (dc5 & UINT32_C(0xe0)) == UINT32_C(0x80) &&
           (dc5 & UINT32_C(0x4)) == 0 &&
           (imx6_ipu1_trace_word(s, IPU_DISP_GEN) &
            UINT32_C(0x01000000)) != 0;
}

/*
 * Decode the exact fields written by Pioneer ipu_param_mem.h:
 *   word0[125:137] width-1, word0[138:149] height-1,
 *   word0[107:109] bpp, word1[0:28]/[29:57] EBA0/EBA1,
 *   word1[85:88] pixel format, word1[102:115] stride-1.
 *
 * The diagnostic only accepts the observed 800x480 RGB565 contract.  It
 * returns false for inactive, incomplete, or other-format CPMEM state rather
 * than treating an arbitrary IPU channel as a framebuffer.
 */
bool imx6_ipu1_trace_bg23_scanout_get(const IMX6IPU1TraceState *s,
                                      IMX6IPU1BG23Scanout *scanout)
{
    const uint32_t *entry;
    uint32_t width, height, stride, bpp, pixel_format;
    uint32_t eba0, eba1;
    bool current;

    if (!s || !scanout || !imx6_ipu1_bg23_scan_active(s)) {
        return false;
    }
    entry = &s->words[(IPU_CPMEM_REG_BASE +
                       IPU_BG23_CPMEM_CHANNEL * IPU_BG23_CPMEM_ENTRY_SIZE) /
                      sizeof(uint32_t)];
    width = (((entry[3] >> 29) & 0x7) | ((entry[4] & 0x3ff) << 3)) + 1;
    height = ((entry[4] >> 10) & 0xfff) + 1;
    bpp = (entry[3] >> 11) & 0x7;
    pixel_format = (entry[IPU_BG23_CPMEM_WORD1 + 2] >> 21) & 0xf;
    stride = ((entry[IPU_BG23_CPMEM_WORD1 + 3] >> 6) & 0x3fff) + 1;
    if (width != IPU_BG23_SCANOUT_WIDTH ||
        height != IPU_BG23_SCANOUT_HEIGHT ||
        stride != IPU_BG23_SCANOUT_STRIDE ||
        bpp != IPU_BG23_CPMEM_RGB565_BPP ||
        pixel_format != IPU_BG23_CPMEM_RGB565_FMT) {
        return false;
    }

    /* EBA fields are stored as physical address / 8 across two words. */
    eba0 = entry[IPU_BG23_CPMEM_WORD1] & 0x1fffffff;
    eba1 = (entry[IPU_BG23_CPMEM_WORD1] >> 29) |
           ((entry[IPU_BG23_CPMEM_WORD1 + 1] & 0x03ffffff) << 3);
    current = (imx6_ipu1_trace_word(s, IPU_CHA_CUR_BUF_23) &
               IPU_BG_SYNC_BIT) != 0;
    scanout->address = (hwaddr)(current ? eba1 : eba0) << 3;
    scanout->width = width;
    scanout->height = height;
    scanout->stride = stride;
    scanout->buffer = current;
    return true;
}

static uint32_t imx6_ipu1_bg23_ready(const IMX6IPU1TraceState *s)
{
    uint32_t ready = 0;

    if (imx6_ipu1_trace_word(s, IPU_CHA_BUF0_RDY_23) & IPU_BG_SYNC_BIT) {
        ready |= UINT32_C(1);
    }
    if (imx6_ipu1_trace_word(s, IPU_CHA_BUF1_RDY_23) & IPU_BG_SYNC_BIT) {
        ready |= UINT32_C(2);
    }
    return ready;
}

static void imx6_ipu1_bg23_route_irq(IMX6IPU1TraceState *s,
                                     const char *reason)
{
    uint32_t stat1 = imx6_ipu1_trace_word(s, IPU_INT_STAT_1);
    uint32_t ctrl1 = imx6_ipu1_trace_word(s, IPU_INT_CTRL_1);
    uint32_t stat2 = imx6_ipu1_trace_word(s, IPUV3H_REG_BASE + 0x0204);
    uint32_t ctrl2 = imx6_ipu1_trace_word(s, IPUV3H_REG_BASE + 0x0040);
    bool asserted = (stat1 & ctrl1 & IPU_BG_SYNC_BIT) != 0;

    qemu_set_irq(s->bg23_irq, asserted);
    qemu_log_mask(LOG_GUEST_ERROR,
                  TYPE_IMX6_IPU1_TRACE ": BG23 IRQ_ROUTE reason=%s "
                  "ctrl1=0x%08x stat1=0x%08x ctrl2=0x%08x stat2=0x%08x "
                  "gic6=%u vtime_ns=%" PRIi64 "\n",
                  reason, ctrl1, stat1, ctrl2, stat2, asserted,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

static void imx6_ipu1_bg23_cancel(IMX6IPU1TraceState *s,
                                   const char *reason)
{
    if (s->bg23_stage == IPU_BG23_STAGE_NONE) {
        return;
    }
    timer_del(s->bg23_frame_timer);
    qemu_log_mask(LOG_GUEST_ERROR,
                  TYPE_IMX6_IPU1_TRACE ": BG23 FRAME_BOUNDARY cancelled "
                  "reason=%s gen=%" PRIu64 " stage=%u vtime_ns=%" PRIi64 "\n",
                  reason, s->bg23_scheduled_generation, s->bg23_stage,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    s->bg23_stage = IPU_BG23_STAGE_NONE;
}

static int imx6_ipu1_bg23_pick_buffer(const IMX6IPU1TraceState *s)
{
    uint32_t ready = imx6_ipu1_bg23_ready(s);
    bool current = (imx6_ipu1_trace_word(s, IPU_CHA_CUR_BUF_23) &
                    IPU_BG_SYNC_BIT) != 0;

    /* Prefer the non-current ready buffer; otherwise the only ready buffer. */
    if (ready & (current ? UINT32_C(1) : UINT32_C(2))) {
        return current ? 0 : 1;
    }
    if (ready & UINT32_C(1)) {
        return 0;
    }
    if (ready & UINT32_C(2)) {
        return 1;
    }
    return -1;
}

static void imx6_ipu1_bg23_try_arm(IMX6IPU1TraceState *s,
                                    const char *reason)
{
    bool active;
    int buffer;

    if (!s->bg23_eof_diagnostic_enable ||
        s->bg23_stage != IPU_BG23_STAGE_NONE) {
        return;
    }
    active = imx6_ipu1_bg23_scan_active(s);
    if (active != s->bg23_scan_active_last) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      TYPE_IMX6_IPU1_TRACE ": BG23 CONFIG_ELIGIBLE active=%u "
                      "conf=0x%08x idmac=0x%08x srm=0x%08x dc5=0x%08x "
                      "disp_gen=0x%08x vtime_ns=%" PRIi64 "\n",
                      active, imx6_ipu1_trace_word(s, IPU_CONF),
                      imx6_ipu1_trace_word(s, IPU_IDMAC_CHA_EN_23),
                      imx6_ipu1_trace_word(s, IPU_SRM_PRI2),
                      imx6_ipu1_trace_word(s, IPU_DC_WR_CH_CONF_5),
                      imx6_ipu1_trace_word(s, IPU_DISP_GEN),
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        s->bg23_scan_active_last = active;
    }
    if (!active) {
        return;
    }
    buffer = imx6_ipu1_bg23_pick_buffer(s);
    if (buffer < 0) {
        return;
    }
    s->bg23_scheduled_buffer = buffer;
    s->bg23_scheduled_generation = ++s->bg23_generation;
    s->bg23_stage = IPU_BG23_STAGE_BOUNDARY;
    timer_mod(s->bg23_frame_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              IPU_BG23_FRAME_NS);
    qemu_log_mask(LOG_GUEST_ERROR,
                  TYPE_IMX6_IPU1_TRACE ": BG23 ARM reason=%s buffer=%d "
                  "gen=%" PRIu64 " current=%u ready=0x%x basis=virtual-60hz "
                  "vtime_ns=%" PRIi64 "\n", reason, buffer,
                  s->bg23_scheduled_generation,
                  !!(imx6_ipu1_trace_word(s, IPU_CHA_CUR_BUF_23) &
                     IPU_BG_SYNC_BIT), imx6_ipu1_bg23_ready(s),
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

static void imx6_ipu1_bg23_frame_cb(void *opaque)
{
    IMX6IPU1TraceState *s = opaque;
    uint32_t *ready_word;
    uint32_t old_current;
    bool active;
    bool ready;

    if (!s->bg23_eof_diagnostic_enable ||
        s->bg23_stage == IPU_BG23_STAGE_NONE) {
        return;
    }
    active = imx6_ipu1_bg23_scan_active(s);
    if (s->bg23_stage == IPU_BG23_STAGE_BOUNDARY) {
        ready_word = &s->words[(s->bg23_scheduled_buffer ?
                                 IPU_CHA_BUF1_RDY_23 : IPU_CHA_BUF0_RDY_23) /
                                sizeof(uint32_t)];
        ready = (*ready_word & IPU_BG_SYNC_BIT) != 0;
        qemu_log_mask(LOG_GUEST_ERROR,
                      TYPE_IMX6_IPU1_TRACE ": BG23 FRAME_BOUNDARY "
                      "qualified=%u ready=%u buffer=%u gen=%" PRIu64 " "
                      "basis=virtual-60hz vtime_ns=%" PRIi64 "\n",
                      active, ready, s->bg23_scheduled_buffer,
                      s->bg23_scheduled_generation,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        if (!active || !ready) {
            s->bg23_stage = IPU_BG23_STAGE_NONE;
            return;
        }
        old_current = imx6_ipu1_trace_word(s, IPU_CHA_CUR_BUF_23);
        *ready_word &= ~IPU_BG_SYNC_BIT;
        s->words[IPU_CHA_CUR_BUF_23 / sizeof(uint32_t)] =
            (old_current & ~IPU_BG_SYNC_BIT) |
            (s->bg23_scheduled_buffer ? IPU_BG_SYNC_BIT : 0);
        qemu_log_mask(LOG_GUEST_ERROR,
                      TYPE_IMX6_IPU1_TRACE ": BG23 CONSUME buffer=%u "
                      "ready_before=1 ready_after=%u current_before=%u "
                      "current_after=%u gen=%" PRIu64 " vtime_ns=%" PRIi64 "\n",
                      s->bg23_scheduled_buffer,
                      !!(*ready_word & IPU_BG_SYNC_BIT),
                      !!(old_current & IPU_BG_SYNC_BIT),
                      s->bg23_scheduled_buffer,
                      s->bg23_scheduled_generation,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        s->bg23_stage = IPU_BG23_STAGE_EOF;
        timer_mod(s->bg23_frame_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + IPU_BG23_FRAME_NS);
        return;
    }

    if (!active || s->bg23_stage != IPU_BG23_STAGE_EOF) {
        s->bg23_stage = IPU_BG23_STAGE_NONE;
        return;
    }
    old_current = imx6_ipu1_trace_word(s, IPU_INT_STAT_1);
    s->words[IPU_INT_STAT_1 / sizeof(uint32_t)] =
        old_current | IPU_BG_SYNC_BIT;
    s->bg23_stage = IPU_BG23_STAGE_NONE;
    qemu_log_mask(LOG_GUEST_ERROR,
                  TYPE_IMX6_IPU1_TRACE ": BG23 EOF23_LATCH stat_before=0x%08x "
                  "stat_after=0x%08x gen=%" PRIu64 " vtime_ns=%" PRIi64 "\n",
                  old_current, imx6_ipu1_trace_word(s, IPU_INT_STAT_1),
                  s->bg23_scheduled_generation,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    imx6_ipu1_bg23_route_irq(s, "eof23-latch");

    /*
     * A ready buffer may have been queued while this just-consumed buffer
     * awaited EOF.  EOF belongs to the consumed buffer and must be delivered
     * before starting the next boundary; then preserve the queued buffer for
     * the next frame.
     */
    imx6_ipu1_bg23_try_arm(s, "eof23-latch");
}

static void imx6_ipu1_bg23_reconcile(IMX6IPU1TraceState *s,
                                      const char *reason)
{
    if (!s->bg23_eof_diagnostic_enable) {
        return;
    }
    if (s->bg23_stage != IPU_BG23_STAGE_NONE &&
        !imx6_ipu1_bg23_scan_active(s)) {
        imx6_ipu1_bg23_cancel(s, reason);
    }
    imx6_ipu1_bg23_try_arm(s, reason);
    imx6_ipu1_bg23_route_irq(s, reason);
}

static int imx6_ipu1_trace_sync_bank(hwaddr offset, bool *is_ctrl)
{
    hwaddr aligned = offset & ~UINT64_C(3);
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(imx6_ipu1_sync_banks); i++) {
        unsigned int bank = imx6_ipu1_sync_banks[i];

        if (aligned == IPUV3H_REG_BASE + 0x003c + 4 * (bank - 1)) {
            *is_ctrl = true;
            return bank;
        }
        if (aligned == IPUV3H_REG_BASE + 0x0200 + 4 * (bank - 1)) {
            *is_ctrl = false;
            return bank;
        }
    }
    return 0;
}

static const char *imx6_ipu1_trace_register(hwaddr offset)
{
    switch (offset & ~UINT64_C(3)) {
    case IPU_CONF:
        return "IPU_CONF";
    case IPU_SRM_PRI2:
        return "IPU_SRM_PRI2";
    case IPU_DISP_GEN:
        return "IPU_DISP_GEN";
    case IPU_INT_CTRL_1:
        return "IPU_INT_CTRL_1";
    case IPU_INT_STAT_1:
        return "IPU_INT_STAT_1";
    case IPU_CHA_CUR_BUF_23:
        return "IPU_CHA_CUR_BUF_23";
    case IPU_CHA_DB_MODE_SEL_23:
        return "IPU_CHA_DB_MODE_SEL_23";
    case IPU_CHA_TRB_MODE_SEL_23:
        return "IPU_CHA_TRB_MODE_SEL_23";
    case IPU_CHA_TRIPLE_CUR_BUF_23:
        return "IPU_CHA_TRIPLE_CUR_BUF_23";
    case IPU_CHA_BUF0_RDY_23:
        return "IPU_CHA_BUF0_RDY_23";
    case IPU_CHA_BUF1_RDY_23:
        return "IPU_CHA_BUF1_RDY_23";
    case IPU_CHA_BUF2_RDY_23:
        return "IPU_CHA_BUF2_RDY_23";
    case IPU_GPR:
        return "IPU_GPR";
    case IPU_IDMAC_CHA_EN_23:
        return "IDMAC_CHA_EN_23";
    case IPU_DI0_GENERAL:
        return "DI0_GENERAL";
    case IPU_DI0_STAT:
        return "DI0_STAT";
    case IPU_DC_WR_CH_CONF_5:
        return "DC_WR_CH_CONF_5";
    case IPU_DC_GEN:
        return "DC_GEN";
    case IPU_DC_STAT:
        return "DC_STAT";
    default:
        if (offset >= IPU_DMFC_FIRST && offset < IPU_DMFC_LAST) {
            return "DMFC";
        }
        return NULL;
    }
}

static bool imx6_ipu1_trace_interesting(hwaddr offset)
{
    bool is_ctrl;

    return imx6_ipu1_trace_sync_bank(offset, &is_ctrl) ||
           imx6_ipu1_trace_register(offset) != NULL;
}

static bool imx6_ipu1_trace_reserve_event(IMX6IPU1TraceState *s)
{
    if (s->trace_events < IPU_TRACE_MAX_EVENTS) {
        s->trace_events++;
        return true;
    }
    if (!s->trace_suppressed) {
        s->trace_suppressed = true;
        qemu_log_mask(LOG_GUEST_ERROR,
                      TYPE_IMX6_IPU1_TRACE ": trace-cap=%u; further "
                      "passive IPU events suppressed\n", IPU_TRACE_MAX_EVENTS);
    }
    return false;
}

static void imx6_ipu1_trace_log(IMX6IPU1TraceState *s, const char *operation,
                                 hwaddr offset, unsigned size, uint32_t value)
{
    const char *name = imx6_ipu1_trace_register(offset);
    bool is_ctrl = false;
    int bank = imx6_ipu1_trace_sync_bank(offset, &is_ctrl);
    uint32_t ctrl, stat;

    if (!imx6_ipu1_trace_interesting(offset) ||
        !imx6_ipu1_trace_reserve_event(s)) {
        return;
    }
    if (bank) {
        ctrl = s->words[(IPUV3H_REG_BASE + 0x003c + 4 * (bank - 1)) / 4];
        stat = s->words[(IPUV3H_REG_BASE + 0x0200 + 4 * (bank - 1)) / 4];
        qemu_log_mask(LOG_GUEST_ERROR,
                      TYPE_IMX6_IPU1_TRACE ": event=%s reg=IPU_INT_%s_%d "
                      "offset=0x%" HWADDR_PRIx " size=%u value=0x%08x "
                      "pc=0x%08x cpu=%d vtime_ns=%" PRIi64 " "
                      "sync{bank=%d,ctrl=0x%08x,stat=0x%08x,masked=0x%08x,w1c=%u}\n",
                      operation, is_ctrl ? "CTRL" : "STAT", bank, offset,
                      size, value, imx6_ipu1_trace_pc(),
                      current_cpu ? current_cpu->cpu_index : -1,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), bank, ctrl, stat,
                      stat & ctrl, !is_ctrl && !strcmp(operation, "write"));
        return;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  TYPE_IMX6_IPU1_TRACE ": event=%s reg=%s offset=0x%"
                  HWADDR_PRIx " size=%u value=0x%08x pc=0x%08x cpu=%d "
                  "vtime_ns=%" PRIi64 " bg_sync{conf=%u,idmac=%u,ready0=%u,"
                  "ready1=%u,irq23=%u,status23=%u}\n",
                  operation, name, offset, size, value, imx6_ipu1_trace_pc(),
                  current_cpu ? current_cpu->cpu_index : -1,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                  ((s->words[IPU_CONF / 4] & IPU_BG_SYNC_CONF_MASK) ==
                   IPU_BG_SYNC_CONF_MASK),
                  !!(s->words[IPU_IDMAC_CHA_EN_23 / 4] & IPU_BG_SYNC_BIT),
                  !!(s->words[IPU_CHA_BUF0_RDY_23 / 4] & IPU_BG_SYNC_BIT),
                  !!(s->words[IPU_CHA_BUF1_RDY_23 / 4] & IPU_BG_SYNC_BIT),
                  !!(s->words[IPU_INT_CTRL_1 / 4] & IPU_BG_SYNC_BIT),
                  !!(s->words[IPU_INT_STAT_1 / 4] & IPU_BG_SYNC_BIT));
}

static uint64_t imx6_ipu1_trace_read(void *opaque, hwaddr offset,
                                     unsigned size)
{
    IMX6IPU1TraceState *s = opaque;
    uint32_t word;
    unsigned shift;

    if (offset + size > IMX6_IPU1_TRACE_SIZE || size > sizeof(word) ||
        (offset % sizeof(word)) + size > sizeof(word)) {
        return 0;
    }
    word = s->words[offset / sizeof(word)];
    shift = (offset % sizeof(word)) * 8;
    imx6_ipu1_trace_log(s, "read", offset, size, word >> shift);
    return extract32(word, shift, size * 8);
}

static void imx6_ipu1_trace_write(void *opaque, hwaddr offset, uint64_t value,
                                  unsigned size)
{
    IMX6IPU1TraceState *s = opaque;
    bool is_ctrl;
    hwaddr aligned;
    uint32_t old, word, written;
    unsigned shift;

    if (offset + size > IMX6_IPU1_TRACE_SIZE || size > sizeof(word) ||
        (offset % sizeof(word)) + size > sizeof(word)) {
        return;
    }
    aligned = offset & ~UINT64_C(3);
    shift = (offset % sizeof(word)) * 8;
    old = s->words[offset / sizeof(word)];
    word = deposit32(old, shift, size * 8, value);
    written = deposit32(0, shift, size * 8, value);
    if (imx6_ipu1_trace_sync_bank(offset, &is_ctrl) && !is_ctrl) {
        /* Pioneer ipu_clear_irq() uses W1C on every sync status bank. */
        s->words[offset / sizeof(word)] = old & ~written;
    } else if (aligned == IPU_CHA_BUF0_RDY_23 ||
               aligned == IPU_CHA_BUF1_RDY_23 ||
               aligned == IPU_CHA_BUF2_RDY_23) {
        /* ipu_select_buffer sets; _ipu_clear_buffer_ready arms IPU_GPR. */
        if (s->words[IPU_GPR / sizeof(word)] == IPU_READY_CLEAR_GPR) {
            s->words[offset / sizeof(word)] = old & ~written;
        } else {
            s->words[offset / sizeof(word)] = old | written;
        }
    } else {
        s->words[offset / sizeof(word)] = word;
    }
    imx6_ipu1_trace_log(s, "write", offset, size, value);
    if (s->bg23_eof_diagnostic_enable) {
        if (aligned == IPU_CHA_BUF0_RDY_23 ||
            aligned == IPU_CHA_BUF1_RDY_23) {
            /*
             * A rearm may replace a buffer waiting for its boundary, but it
             * cannot revoke an EOF already owed to a consumed buffer.  Linux
             * normally queues the next READY during this EOF interval.
             */
            if (s->bg23_stage == IPU_BG23_STAGE_BOUNDARY) {
                imx6_ipu1_bg23_cancel(s, "ready-write");
            }
        }
        imx6_ipu1_bg23_reconcile(s, "guest-write");
    }
}

static const MemoryRegionOps imx6_ipu1_trace_ops = {
    .read = imx6_ipu1_trace_read,
    .write = imx6_ipu1_trace_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = sizeof(uint32_t),
    },
};

static void imx6_ipu1_trace_reset(DeviceState *dev)
{
    IMX6IPU1TraceState *s = IMX6_IPU1_TRACE(dev);

    if (s->bg23_frame_timer) {
        timer_del(s->bg23_frame_timer);
    }
    memset(s->words, 0, IMX6_IPU1_TRACE_SIZE);
    s->trace_events = 0;
    s->trace_suppressed = false;
    s->bg23_generation = 0;
    s->bg23_scheduled_generation = 0;
    s->bg23_scheduled_buffer = 0;
    s->bg23_stage = IPU_BG23_STAGE_NONE;
    s->bg23_scan_active_last = false;
    if (s->bg23_eof_diagnostic_enable) {
        qemu_set_irq(s->bg23_irq, 0);
    }
}

static void imx6_ipu1_trace_init(Object *obj)
{
    IMX6IPU1TraceState *s = IMX6_IPU1_TRACE(obj);

    s->words = g_new0(uint32_t, IMX6_IPU1_TRACE_WORDS);
    memory_region_init_io(&s->iomem, obj, &imx6_ipu1_trace_ops, s,
                          TYPE_IMX6_IPU1_TRACE, IMX6_IPU1_TRACE_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->bg23_irq);
    s->bg23_frame_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                       imx6_ipu1_bg23_frame_cb, s);
}

static void imx6_ipu1_trace_finalize(Object *obj)
{
    IMX6IPU1TraceState *s = IMX6_IPU1_TRACE(obj);

    if (s->bg23_frame_timer) {
        timer_free(s->bg23_frame_timer);
        s->bg23_frame_timer = NULL;
    }
    g_free(s->words);
}

static void imx6_ipu1_trace_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, imx6_ipu1_trace_reset);
    dc->desc = "i.MX6 IPU1 trace-only diagnostic window";
}

static const TypeInfo imx6_ipu1_trace_info = {
    .name = TYPE_IMX6_IPU1_TRACE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMX6IPU1TraceState),
    .instance_init = imx6_ipu1_trace_init,
    .instance_finalize = imx6_ipu1_trace_finalize,
    .class_init = imx6_ipu1_trace_class_init,
};

static void imx6_ipu1_trace_register_types(void)
{
    type_register_static(&imx6_ipu1_trace_info);
}

type_init(imx6_ipu1_trace_register_types)
