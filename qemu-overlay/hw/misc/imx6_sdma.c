/*
 * Minimal i.MX6 SDMA command-channel model for the XDJ-XZ.
 *
 * Pioneer Linux 3.0.101 uses channel zero synchronously while loading its
 * SDMA firmware and configuring DMA contexts.  It supplies a guest-memory
 * CCB/BD, starts channel zero, polls INTR[0], then clears that bit by W1C.
 * This model implements that setup contract.  Its optional cyclic path
 * provides Pioneer PCM descriptor-to-IRQ handoff and transfers the bounded
 * PCM period into the matching SSI endpoint.  It neither executes arbitrary
 * SDMA scripts nor supplies host audio.
 */
#include "qemu/osdep.h"
#include "hw/misc/imx6_sdma.h"
#include "hw/misc/imx6_ssi.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "trace.h"

#define SDMA_H_C0PTR       0x000
#define SDMA_H_INTR        0x004
#define SDMA_H_STATSTOP    0x008
#define SDMA_H_START       0x00c
#define SDMA_H_RESET       0x024
#define SDMA_H_EVTERR      0x028
#define SDMA_H_CONFIG      0x038
#define SDMA_CHN0ADDR      0x05c
#define SDMA_CHNENBL0_V2   0x200

#define SDMA_CH0           (1U << 0)
#define SDMA_CHN0ADDR_VALUE 0x4050
#define SDMA_CONFIG_CSM    0x3

#define BD_DONE            0x01
#define BD_WRAP            0x02
#define BD_CONT            0x04
#define BD_INTR            0x08
#define BD_EXTD            0x80
#define BD_SETUP_FLAGS     (BD_DONE | BD_WRAP | BD_INTR | BD_EXTD)
#define BD_CYCLIC_FLAGS    (BD_DONE | BD_CONT | BD_INTR | BD_EXTD)
#define C0_SETDM           0x01
#define C0_SETPM           0x04

/* Pioneer streams use 44.1kHz stereo, 32-bit slots. */
#define SDMA_PCM_RATE_HZ            44100
#define SDMA_PCM_CHANNELS            2
#define SDMA_PCM_SLOT_BYTES          4
/* The original aplay smoke path uses 16 KiB BDs; larger BDs remain deferred. */
#define SDMA_PCM_MAX_PERIOD_BYTES    16384
/* Keep a delayed audio timer bounded while preserving descriptor ownership. */
#define SDMA_AUDIO_MAX_CATCHUP_PERIODS 8
#define SDMA_MAX_CYCLIC_DESCRIPTORS IMX6_SDMA_MAX_CYCLIC_DESCRIPTORS

/* Pioneer mx6: SSI1 RX0/TX0 are events 37/38; SSI2 RX0/TX0 41/42. */
#define SDMA_EVENT_SSI1_RX0 37
#define SDMA_EVENT_SSI1_TX0 38
#define SDMA_EVENT_SSI2_RX0 41
#define SDMA_EVENT_SSI2_TX0 42
#define SDMA_CONTEXT_WORDS  32
#define SDMA_CONTEXT_DEST_BASE 2048
#define SDMA_CONTEXT_DEST_STRIDE SDMA_CONTEXT_WORDS
#define SSI1_SRX0_ADDR      0x02028008
#define SSI2_SRX0_ADDR      0x0202c008
#define SSI1_STX0_ADDR      0x02028000
#define SSI2_STX0_ADDR      0x0202c000

enum {
    SDMA_AUDIO_UNKNOWN,
    SDMA_AUDIO_PLAYBACK,
    SDMA_AUDIO_CAPTURE,
};

typedef struct SDMACCB {
    uint32_t current_bd_ptr;
    uint32_t base_bd_ptr;
    uint32_t unused[2];
} SDMACCB;

typedef struct SDMABD {
    uint32_t mode_count;
    uint32_t buffer_addr;
    uint32_t ext_buffer_addr;
} SDMABD;

/* Exact packed Pioneer context shape: state (2 words), gReg[8], then 22. */
typedef struct SDMAContext {
    uint32_t state[2];
    uint32_t greg[8];
    uint32_t rest[22];
} SDMAContext;

static bool imx6_sdma_read_guest(IMX6SDMAState *s, hwaddr address,
                                 void *buffer, size_t length)
{
    if (s->performance_diagnostic_enable) {
        s->performance_guest_read_ops++;
        s->performance_guest_read_bytes += length;
    }
    return address_space_read(&address_space_memory, address,
                              MEMTXATTRS_UNSPECIFIED, buffer, length) ==
           MEMTX_OK;
}

static bool imx6_sdma_write_guest(IMX6SDMAState *s, hwaddr address,
                                  const void *buffer, size_t length)
{
    if (s->performance_diagnostic_enable) {
        s->performance_guest_write_ops++;
        s->performance_guest_write_bytes += length;
    }
    return address_space_write(&address_space_memory, address,
                               MEMTXATTRS_UNSPECIFIED, buffer, length) ==
           MEMTX_OK;
}

static void imx6_sdma_update_irq(IMX6SDMAState *s)
{
    bool level = !!(s->intr_status & ~SDMA_CH0);

    /* Pioneer IRQ handler explicitly ignores command channel zero. */
    if (s->performance_diagnostic_enable) {
        s->performance_irq_update_calls++;
        if (level != s->performance_irq_level) {
            s->performance_irq_transitions++;
        }
        s->performance_irq_level = level;
    }
    qemu_set_irq(s->irq, level);
}

static uint32_t imx6_sdma_cyclic_active_mask(const IMX6SDMAState *s)
{
    uint32_t mask = 0;
    unsigned int channel;

    for (channel = 1; channel < IMX6_SDMA_CYCLIC_CHANNELS; channel++) {
        if (s->cyclic_active[channel]) {
            mask |= 1U << channel;
        }
    }
    return mask;
}

static bool imx6_sdma_any_cyclic_active(const IMX6SDMAState *s)
{
    return imx6_sdma_cyclic_active_mask(s) != 0;
}

static bool imx6_sdma_valid_period_bytes(uint32_t bytes)
{
    return bytes && bytes <= SDMA_PCM_MAX_PERIOD_BYTES;
}

static uint64_t imx6_sdma_period_ns(uint32_t bytes)
{
    return (NANOSECONDS_PER_SECOND * (uint64_t)bytes) /
        (SDMA_PCM_RATE_HZ * SDMA_PCM_CHANNELS * SDMA_PCM_SLOT_BYTES);
}

static void imx6_sdma_schedule_period(IMX6SDMAState *s)
{
    uint64_t due = UINT64_MAX;
    unsigned int channel;

    for (channel = 1; channel < IMX6_SDMA_CYCLIC_CHANNELS; channel++) {
        if (s->cyclic_active[channel] && s->cyclic_next_due_ns[channel] &&
            s->cyclic_next_due_ns[channel] < due) {
            due = s->cyclic_next_due_ns[channel];
        }
    }
    if (!imx6_sdma_any_cyclic_active(s) || due == UINT64_MAX) {
        return;
    }
    timer_mod(s->period_timer, due);
}

static void imx6_sdma_stop_cyclic_channel(IMX6SDMAState *s,
                                           unsigned int channel)
{
    s->cyclic_active[channel] = false;
    s->cyclic_bd_count[channel] = 0;
    s->cyclic_next[channel] = 0;
    s->cyclic_bd_base[channel] = 0;
    s->cyclic_period_bytes[channel] = 0;
    s->cyclic_next_due_ns[channel] = 0;
    s->cyclic_audio_direction[channel] = SDMA_AUDIO_UNKNOWN;
    s->cyclic_audio_peripheral[channel] = 0;
}

/*
 * A cyclic BD which retains its script flags but has released BD_DONE has
 * not been returned to SDMA by the Linux IRQ path yet.  Hardware waits for
 * ownership; it does not terminate the whole channel or invent a completion.
 */
static bool imx6_sdma_cyclic_bd_deferred(IMX6SDMAState *s,
                                         unsigned int channel,
                                         const SDMABD *bd, uint8_t status)
{
    if ((status & (BD_CONT | BD_INTR | BD_EXTD)) !=
        (BD_CONT | BD_INTR | BD_EXTD) ||
        !imx6_sdma_valid_period_bytes(bd->mode_count & 0xffff) ||
        !bd->buffer_addr || (status & BD_DONE)) {
        return false;
    }
    s->cyclic_deferrals[channel]++;
    if (s->cyclic_deferrals[channel] == 1 ||
        !(s->cyclic_deferrals[channel] % 1024)) {
        trace_imx6_sdma_cyclic_defer(channel, s->cyclic_deferrals[channel],
                                     status);
    }
    return true;
}

/*
 * Pioneer imx-sdma's sdma_prep_dma_cyclic() fixes each ring entry's buffer
 * address, count, command, and static flags.  Its loop IRQ handler only
 * returns ownership with BD_DONE, then the ALSA callback rearms BD_DONE.
 *
 * For those opt-in audio rings, retain the static descriptor fields captured
 * at START and re-read only the live ownership word.  A changed mode/count
 * immediately falls back to the full descriptor path; this never creates a
 * completion or hides an unavailable descriptor.
 */
static bool imx6_sdma_read_cyclic_bd(IMX6SDMAState *s, unsigned int channel,
                                     unsigned int index, hwaddr bd_addr,
                                     SDMABD *bd)
{
    uint32_t mode_count;

    if (!s->cyclic_static_metadata[channel]) {
        return imx6_sdma_read_guest(s, bd_addr, bd, sizeof(*bd));
    }
    if (!imx6_sdma_read_guest(s, bd_addr, &mode_count, sizeof(mode_count))) {
        return false;
    }
    if ((mode_count & ~(BD_DONE << 16)) !=
        s->cyclic_bd_mode_template[channel][index]) {
        s->cyclic_static_metadata[channel] = false;
        return imx6_sdma_read_guest(s, bd_addr, bd, sizeof(*bd));
    }
    bd->mode_count = mode_count;
    bd->buffer_addr = s->cyclic_bd_buffer_addr[channel][index];
    bd->ext_buffer_addr = 0;
    return true;
}

static uint8_t imx6_sdma_audio_direction(const IMX6SDMAState *s,
                                         unsigned int channel)
{
    static const unsigned int capture_events[] = {
        SDMA_EVENT_SSI1_RX0, SDMA_EVENT_SSI2_RX0,
    };
    static const unsigned int playback_events[] = {
        SDMA_EVENT_SSI1_TX0, SDMA_EVENT_SSI2_TX0,
    };
    size_t index;

    if (!s->cyclic_audio_enable) {
        return SDMA_AUDIO_UNKNOWN;
    }
    for (index = 0; index < G_N_ELEMENTS(capture_events); index++) {
        unsigned int event = capture_events[index];
        uint32_t enabled = s->regs[(SDMA_CHNENBL0_V2 + event * 4) /
                                   sizeof(uint32_t)];

        if (enabled & (1U << channel)) {
            return SDMA_AUDIO_CAPTURE;
        }
    }
    for (index = 0; index < G_N_ELEMENTS(playback_events); index++) {
        unsigned int event = playback_events[index];
        uint32_t enabled = s->regs[(SDMA_CHNENBL0_V2 + event * 4) /
                                   sizeof(uint32_t)];

        if (enabled & (1U << channel)) {
            return SDMA_AUDIO_PLAYBACK;
        }
    }
    return SDMA_AUDIO_UNKNOWN;
}

static uint8_t imx6_sdma_direction_for_peripheral(uint32_t address)
{
    switch (address) {
    case SSI1_SRX0_ADDR:
    case SSI2_SRX0_ADDR:
        return SDMA_AUDIO_CAPTURE;
    case SSI1_STX0_ADDR:
    case SSI2_STX0_ADDR:
        return SDMA_AUDIO_PLAYBACK;
    default:
        return SDMA_AUDIO_UNKNOWN;
    }
}

/*
 * Channel contexts are loaded through CH0 C0_SETDM.  gReg[6] is shp_addr in
 * Pioneer drivers/dma/imx-sdma.c, and carries the SSI STX0/SRX0 address.
 * That is the authoritative direction signal after CHNENBL has been changed
 * by ownership management.
 */
static void imx6_sdma_record_context(IMX6SDMAState *s, const SDMABD *bd)
{
    SDMAContext context;
    uint32_t destination = bd->ext_buffer_addr;
    uint32_t channel;
    uint32_t pc;
    uint32_t peripheral;

    if (destination < SDMA_CONTEXT_DEST_BASE ||
        (destination - SDMA_CONTEXT_DEST_BASE) % SDMA_CONTEXT_DEST_STRIDE) {
        return;
    }
    channel = (destination - SDMA_CONTEXT_DEST_BASE) /
        SDMA_CONTEXT_DEST_STRIDE;
    if (channel == 0 || channel >= IMX6_SDMA_CYCLIC_CHANNELS ||
        !imx6_sdma_read_guest(s, bd->buffer_addr, &context,
                              sizeof(context))) {
        return;
    }
    pc = context.state[0] & 0x3fff;
    peripheral = context.greg[6];
    s->cyclic_audio_direction[channel] =
        imx6_sdma_direction_for_peripheral(peripheral);
    s->cyclic_audio_peripheral[channel] = peripheral;
    trace_imx6_sdma_audio_context(channel, pc, peripheral,
                                  s->cyclic_audio_direction[channel]);
}

static IMX6SSIState *imx6_sdma_ssi_for_peripheral(IMX6SDMAState *s,
                                                   uint32_t peripheral)
{
    switch (peripheral) {
    case SSI1_SRX0_ADDR:
    case SSI1_STX0_ADDR:
        return s->ssi[0];
    case SSI2_SRX0_ADDR:
    case SSI2_STX0_ADDR:
        return s->ssi[1];
    default:
        return NULL;
    }
}

static bool imx6_sdma_start_cyclic_channel(IMX6SDMAState *s,
                                            unsigned int channel)
{
    SDMACCB ccb;
    unsigned int index;
    uint32_t ccb_addr;
    uint32_t first_period_bytes = 0;

    if (!s->cyclic_diagnostic_enable || channel == 0 ||
        channel >= IMX6_SDMA_CYCLIC_CHANNELS ||
        !s->regs[SDMA_H_C0PTR / sizeof(uint32_t)]) {
        return false;
    }

    ccb_addr = s->regs[SDMA_H_C0PTR / sizeof(uint32_t)] +
        channel * sizeof(SDMACCB);
    if (!imx6_sdma_read_guest(s, ccb_addr, &ccb, sizeof(ccb)) ||
        !ccb.current_bd_ptr || ccb.base_bd_ptr != ccb.current_bd_ptr ||
        (ccb.current_bd_ptr & (sizeof(uint32_t) - 1))) {
        return false;
    }

    for (index = 0; index < SDMA_MAX_CYCLIC_DESCRIPTORS; index++) {
        SDMABD bd;
        uint8_t status;
        hwaddr bd_addr = ccb.current_bd_ptr + index * sizeof(SDMABD);

        if (!imx6_sdma_read_guest(s, bd_addr, &bd, sizeof(bd))) {
            return false;
        }
        status = bd.mode_count >> 16;
        if ((status & BD_CYCLIC_FLAGS) != BD_CYCLIC_FLAGS ||
            !imx6_sdma_valid_period_bytes(bd.mode_count & 0xffff) ||
            !bd.buffer_addr) {
            return false;
        }
        if (!first_period_bytes) {
            first_period_bytes = bd.mode_count & 0xffff;
        }
        s->cyclic_bd_mode_template[channel][index] =
            bd.mode_count & ~(BD_DONE << 16);
        s->cyclic_bd_buffer_addr[channel][index] = bd.buffer_addr;
        if (status & BD_WRAP) {
            index++;
            break;
        }
    }
    if (index == SDMA_MAX_CYCLIC_DESCRIPTORS) {
        return false;
    }

    s->cyclic_active[channel] = true;
    s->cyclic_bd_count[channel] = index;
    s->cyclic_next[channel] = 0;
    s->cyclic_bd_base[channel] = ccb.current_bd_ptr;
    s->cyclic_period_bytes[channel] = first_period_bytes;
    s->cyclic_next_due_ns[channel] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
        imx6_sdma_period_ns(first_period_bytes);
    if (s->cyclic_audio_direction[channel] == SDMA_AUDIO_UNKNOWN) {
        s->cyclic_audio_direction[channel] =
            imx6_sdma_audio_direction(s, channel);
    }
    s->cyclic_static_metadata[channel] =
        s->cyclic_static_metadata_fastpath && s->cyclic_audio_enable &&
        s->cyclic_audio_direction[channel] != SDMA_AUDIO_UNKNOWN;
    return true;
}

static bool imx6_sdma_complete_cyclic_channel(IMX6SDMAState *s,
                                               unsigned int channel)
{
    SDMABD bd;
    uint8_t pcm[SDMA_PCM_MAX_PERIOD_BYTES];
    IMX6SSIState *ssi;
    uint8_t status;
    hwaddr bd_addr;

    if (!s->cyclic_active[channel]) {
        return false;
    }
    bd_addr = s->cyclic_bd_base[channel] +
        s->cyclic_next[channel] * sizeof(SDMABD);
    if (!imx6_sdma_read_cyclic_bd(s, channel, s->cyclic_next[channel],
                                  bd_addr, &bd)) {
        trace_imx6_sdma_cyclic_stop(channel, "bd-read", 0, 0, 0);
        imx6_sdma_stop_cyclic_channel(s, channel);
        return false;
    }
    status = bd.mode_count >> 16;
    /* The guest must have offered this descriptor back to SDMA. */
    if (imx6_sdma_cyclic_bd_deferred(s, channel, &bd, status)) {
        return false;
    }
    if ((status & BD_CYCLIC_FLAGS) != BD_CYCLIC_FLAGS ||
        !imx6_sdma_valid_period_bytes(bd.mode_count & 0xffff) ||
        !bd.buffer_addr) {
        trace_imx6_sdma_cyclic_stop(channel, "bd-invalid", status,
                                    bd.mode_count & 0xffff, bd.buffer_addr);
        imx6_sdma_stop_cyclic_channel(s, channel);
        return false;
    }

    ssi = imx6_sdma_ssi_for_peripheral(s,
                                       s->cyclic_audio_peripheral[channel]);
    /*
     * Pioneer imx-ssi leaves cyclic BDs prepared before ALSA starts the
     * stream. Its trigger callback later enables SSIEN and the matching
     * TE/RE plus TDMAE/RDMAE request bits. The real SDMA is paced by those
     * SSI requests, so a prepared-but-disabled endpoint cannot manufacture
     * an IRQ or a period callback.
     *
     * Generic cyclic diagnostics remain unchanged: this applies only to
     * source-identified SSI audio channels.
     */
    if (ssi && s->cyclic_audio_direction[channel] == SDMA_AUDIO_PLAYBACK &&
        !imx6_ssi_dma_request_enabled(ssi, true)) {
        return false;
    }
    if (ssi && s->cyclic_audio_direction[channel] == SDMA_AUDIO_CAPTURE &&
        !imx6_ssi_dma_request_enabled(ssi, false)) {
        return false;
    }
    if (s->cyclic_audio_direction[channel] == SDMA_AUDIO_PLAYBACK && ssi) {
        if (!imx6_sdma_read_guest(s, bd.buffer_addr, pcm,
                                  bd.mode_count & 0xffff)) {
            trace_imx6_sdma_cyclic_stop(channel, "tx-read", status,
                                        bd.mode_count & 0xffff, bd.buffer_addr);
            imx6_sdma_stop_cyclic_channel(s, channel);
            return false;
        }
        imx6_ssi_dma_tx(ssi, pcm, bd.mode_count & 0xffff);
    } else if (s->cyclic_audio_direction[channel] == SDMA_AUDIO_CAPTURE) {
        if (ssi) {
            imx6_ssi_dma_rx(ssi, pcm, bd.mode_count & 0xffff);
        } else {
            memset(pcm, 0, bd.mode_count & 0xffff);
        }
        if (!imx6_sdma_write_guest(s, bd.buffer_addr, pcm,
                                   bd.mode_count & 0xffff)) {
            trace_imx6_sdma_cyclic_stop(channel, "rx-write", status,
                                        bd.mode_count & 0xffff, bd.buffer_addr);
            imx6_sdma_stop_cyclic_channel(s, channel);
            return false;
        }
        s->cyclic_capture_bytes[channel] += bd.mode_count & 0xffff;
    }

    bd.mode_count &= ~(BD_DONE << 16);
    if (!imx6_sdma_write_guest(s, bd_addr, &bd.mode_count,
                               sizeof(bd.mode_count))) {
        imx6_sdma_stop_cyclic_channel(s, channel);
        return false;
    }
    s->cyclic_next[channel] = (s->cyclic_next[channel] + 1) %
        s->cyclic_bd_count[channel];
    /* Schedule from the descriptor about to be owned, not the one just
     * completed.  An unavailable next descriptor remains deferred at the
     * next deadline; no completion is invented. */
    bd_addr = s->cyclic_bd_base[channel] +
        s->cyclic_next[channel] * sizeof(SDMABD);
    if (s->cyclic_static_metadata[channel]) {
        s->cyclic_period_bytes[channel] =
            s->cyclic_bd_mode_template[channel][s->cyclic_next[channel]] &
            0xffff;
    } else if (imx6_sdma_read_guest(s, bd_addr, &bd, sizeof(bd)) &&
               ((bd.mode_count >> 16) & BD_CYCLIC_FLAGS) == BD_CYCLIC_FLAGS &&
               imx6_sdma_valid_period_bytes(bd.mode_count & 0xffff) &&
               bd.buffer_addr) {
        s->cyclic_period_bytes[channel] = bd.mode_count & 0xffff;
    }
    s->cyclic_completions[channel]++;
    /* Bounded progress trace: one proof point, then one per ~1.5 seconds. */
    if (s->cyclic_completions[channel] == 1 ||
        !(s->cyclic_completions[channel] % 1024)) {
        trace_imx6_sdma_cyclic_progress(channel,
                                        s->cyclic_completions[channel]);
    }
    s->intr_status |= 1U << channel;
    return true;
}

static void imx6_sdma_period_timer(void *opaque)
{
    IMX6SDMAState *s = opaque;
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    unsigned int channel;

    if (s->audio_health_enable) {
        s->cyclic_last_timer_virtual_ns = now;
    }

    if (s->performance_diagnostic_enable) {
        s->performance_period_callbacks++;
    }
    for (channel = 1; channel < IMX6_SDMA_CYCLIC_CHANNELS; channel++) {
        uint64_t due = s->cyclic_next_due_ns[channel];
        uint64_t period;
        unsigned int completed = 0;
        bool catch_up;

        if (!s->cyclic_active[channel] || !due || due > now) {
            continue;
        }
        if (s->audio_health_enable && now > due) {
            uint64_t lateness = now - due;

            s->cyclic_late_ns[channel] += lateness;
            s->cyclic_max_late_ns[channel] =
                MAX(s->cyclic_max_late_ns[channel], lateness);
        }
        period = imx6_sdma_period_ns(s->cyclic_period_bytes[channel]);
        if (s->audio_health_enable && now > due + period) {
            s->cyclic_late_periods[channel]++;
        }
        /*
         * The real SSI clocks an offered SDMA descriptor every period even
         * when QEMU reaches this timer late.  The previous generic diagnostic
         * policy threw that elapsed audio away by resynchronising to `now`,
         * which creates repeatable host-audio underruns.  Limit catch-up to
         * source-identified SSI rings and still require BD_DONE ownership for
         * every individual transfer; an unavailable descriptor is deferred,
         * never completed speculatively.
         */
        catch_up = s->cyclic_audio_catchup_enable &&
            s->cyclic_audio_direction[channel] != SDMA_AUDIO_UNKNOWN;
        do {
            if (!imx6_sdma_complete_cyclic_channel(s, channel) ||
                !s->cyclic_active[channel]) {
                /* Ownership deferral retries after one nominal period. */
                s->cyclic_next_due_ns[channel] = now + period;
                break;
            }
            due += period;
            completed++;
            if (due > now) {
                s->cyclic_next_due_ns[channel] = due;
                break;
            }
            if (!catch_up || completed == SDMA_AUDIO_MAX_CATCHUP_PERIODS) {
                s->cyclic_deadline_resyncs[channel]++;
                s->cyclic_next_due_ns[channel] = now + period;
                break;
            }
        } while (true);
        if (s->audio_health_enable) {
            if (completed > 1) {
                s->cyclic_multi_completion_callbacks[channel]++;
            }
            s->cyclic_max_completions_per_callback[channel] =
                MAX(s->cyclic_max_completions_per_callback[channel], completed);
        }
    }
    imx6_sdma_update_irq(s);
    if (imx6_sdma_any_cyclic_active(s)) {
        imx6_sdma_schedule_period(s);
    }
}

static bool imx6_sdma_valid_ch0_setup(IMX6SDMAState *s)
{
    SDMACCB ccb;
    SDMABD bd;
    uint8_t status;
    uint8_t command;

    if (!s->regs[SDMA_H_C0PTR / sizeof(uint32_t)] ||
        s->regs[SDMA_CHN0ADDR / sizeof(uint32_t)] != SDMA_CHN0ADDR_VALUE ||
        (s->regs[SDMA_H_CONFIG / sizeof(uint32_t)] & SDMA_CONFIG_CSM) !=
            SDMA_CONFIG_CSM) {
        return false;
    }
    if (!imx6_sdma_read_guest(s,
                              s->regs[SDMA_H_C0PTR / sizeof(uint32_t)],
                              &ccb, sizeof(ccb)) || !ccb.current_bd_ptr ||
        ccb.base_bd_ptr != ccb.current_bd_ptr ||
        !imx6_sdma_read_guest(s, ccb.current_bd_ptr, &bd, sizeof(bd))) {
        return false;
    }

    status = bd.mode_count >> 16;
    command = bd.mode_count >> 24;
    if ((status & BD_SETUP_FLAGS) != BD_SETUP_FLAGS ||
        (command != C0_SETDM && command != C0_SETPM)) {
        return false;
    }
    if (command == C0_SETDM && (bd.mode_count & 0xffff) ==
        SDMA_CONTEXT_WORDS) {
        imx6_sdma_record_context(s, &bd);
    }
    return true;
}

/*
 * Passive, trace-backend-only descriptor observation.  It neither alters a
 * descriptor nor completes a channel.  The exact Pioneer driver's C array is
 * indexed as struct sdma_channel_control[], so its live stride is the packed
 * 16-byte structure size (drivers/dma/imx-sdma.c), regardless of the stale
 * hardware-comment wording about 128-byte control blocks.
 */
static void imx6_sdma_trace_start(IMX6SDMAState *s, uint32_t bits)
{
    uint32_t channel;
    uint32_t c0ptr = s->regs[SDMA_H_C0PTR / sizeof(uint32_t)];

    trace_imx6_sdma_start(bits, c0ptr);
    for (channel = 0; channel < 32; channel++) {
        SDMACCB ccb = { 0 };
        SDMABD bd = { 0 };
        uint32_t ccb_addr;
        bool ccb_valid;
        bool bd_valid;

        if (!(bits & (1U << channel))) {
            continue;
        }
        ccb_addr = c0ptr + channel * sizeof(SDMACCB);
        ccb_valid = c0ptr && imx6_sdma_read_guest(s, ccb_addr, &ccb,
                                                   sizeof(ccb));
        bd_valid = ccb_valid && ccb.current_bd_ptr &&
            imx6_sdma_read_guest(s, ccb.current_bd_ptr, &bd, sizeof(bd));
        trace_imx6_sdma_channel_descriptor(channel, ccb_addr,
                                            ccb.current_bd_ptr,
                                            ccb.base_bd_ptr, bd.mode_count,
                                            bd.buffer_addr, bd.ext_buffer_addr,
                                            ccb_valid && bd_valid);
    }
}

static uint64_t imx6_sdma_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX6SDMAState *s = opaque;

    if (s->performance_diagnostic_enable && offset == SDMA_H_EVTERR) {
        s->performance_evterr_reads++;
    }
    if (offset == SDMA_H_INTR) {
        return s->intr_status;
    }
    return s->regs[offset / sizeof(uint32_t)];
}

static void imx6_sdma_reset_state(IMX6SDMAState *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    s->intr_status = 0;
    s->rejected_starts = 0;
    memset(s->cyclic_active, 0, sizeof(s->cyclic_active));
    memset(s->cyclic_bd_count, 0, sizeof(s->cyclic_bd_count));
    memset(s->cyclic_next, 0, sizeof(s->cyclic_next));
    memset(s->cyclic_static_metadata, 0, sizeof(s->cyclic_static_metadata));
    memset(s->cyclic_bd_base, 0, sizeof(s->cyclic_bd_base));
    memset(s->cyclic_bd_mode_template, 0,
           sizeof(s->cyclic_bd_mode_template));
    memset(s->cyclic_bd_buffer_addr, 0, sizeof(s->cyclic_bd_buffer_addr));
    memset(s->cyclic_period_bytes, 0, sizeof(s->cyclic_period_bytes));
    memset(s->cyclic_next_due_ns, 0, sizeof(s->cyclic_next_due_ns));
    memset(s->cyclic_late_ns, 0, sizeof(s->cyclic_late_ns));
    memset(s->cyclic_deadline_resyncs, 0,
           sizeof(s->cyclic_deadline_resyncs));
    memset(s->cyclic_audio_direction, 0, sizeof(s->cyclic_audio_direction));
    memset(s->cyclic_audio_peripheral, 0,
           sizeof(s->cyclic_audio_peripheral));
    memset(s->cyclic_completions, 0, sizeof(s->cyclic_completions));
    memset(s->cyclic_deferrals, 0, sizeof(s->cyclic_deferrals));
    memset(s->cyclic_capture_bytes, 0, sizeof(s->cyclic_capture_bytes));
    s->performance_period_callbacks = 0;
    s->performance_start_writes = 0;
    s->performance_intr_w1c_writes = 0;
    s->performance_evterr_reads = 0;
    s->performance_irq_update_calls = 0;
    s->performance_irq_transitions = 0;
    s->performance_guest_read_ops = 0;
    s->performance_guest_read_bytes = 0;
    s->performance_guest_write_ops = 0;
    s->performance_guest_write_bytes = 0;
    s->performance_irq_level = false;
    timer_del(s->period_timer);
    imx6_sdma_update_irq(s);
}

static void imx6_sdma_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    IMX6SDMAState *s = opaque;
    uint32_t bits = value;

    switch (offset) {
    case SDMA_H_INTR:
        /* Pioneer clears the channel bit before examining its descriptor. */
        if (s->performance_diagnostic_enable) {
            s->performance_intr_w1c_writes++;
        }
        s->intr_status &= ~bits;
        imx6_sdma_update_irq(s);
        break;
    case SDMA_H_STATSTOP:
        trace_imx6_sdma_stop(bits, imx6_sdma_cyclic_active_mask(s),
                             s->intr_status);
        for (unsigned int channel = 1;
             channel < IMX6_SDMA_CYCLIC_CHANNELS; channel++) {
            if (bits & (1U << channel)) {
                imx6_sdma_stop_cyclic_channel(s, channel);
            }
        }
        if (!imx6_sdma_any_cyclic_active(s)) {
            timer_del(s->period_timer);
        }
        s->regs[offset / sizeof(uint32_t)] = bits;
        break;
    case SDMA_H_START:
        s->regs[offset / sizeof(uint32_t)] = bits;
        if (s->performance_diagnostic_enable) {
            s->performance_start_writes++;
        }
        if (bits) {
            imx6_sdma_trace_start(s, bits);
        }
        if (bits == SDMA_CH0 && imx6_sdma_valid_ch0_setup(s)) {
            s->intr_status |= SDMA_CH0;
        } else if (bits) {
            uint32_t accepted = 0;
            unsigned int channel;

            for (channel = 1; channel < IMX6_SDMA_CYCLIC_CHANNELS; channel++) {
                if ((bits & (1U << channel)) &&
                    imx6_sdma_start_cyclic_channel(s, channel)) {
                    accepted |= 1U << channel;
                }
            }
            if (accepted) {
                imx6_sdma_schedule_period(s);
            }
            if (bits == accepted) {
                break;
            }
            s->rejected_starts++;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "imx6-sdma: rejected unsupported START=0x%08x\n",
                          bits);
        }
        break;
    case SDMA_H_RESET:
        if (bits) {
            imx6_sdma_reset_state(s);
        }
        break;
    default:
        s->regs[offset / sizeof(uint32_t)] = bits;
        break;
    }
}

static const MemoryRegionOps imx6_sdma_ops = {
    .read = imx6_sdma_read,
    .write = imx6_sdma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void imx6_sdma_reset(DeviceState *dev)
{
    imx6_sdma_reset_state(IMX6_SDMA(dev));
}

static bool imx6_sdma_get_audio_health_enable(Object *obj, Error **errp)
{
    return IMX6_SDMA(obj)->audio_health_enable;
}

static void imx6_sdma_set_audio_health_enable(Object *obj, bool value,
                                               Error **errp)
{
    IMX6_SDMA(obj)->audio_health_enable = value;
}

static void imx6_sdma_init(Object *obj)
{
    IMX6SDMAState *s = IMX6_SDMA(obj);

    memory_region_init_io(&s->iomem, obj, &imx6_sdma_ops, s,
                          TYPE_IMX6_SDMA, IMX6_SDMA_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
    s->period_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, imx6_sdma_period_timer,
                                   s);
    object_property_add_uint64_ptr(obj, "performance-period-callbacks",
                                   &s->performance_period_callbacks,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "performance-start-writes",
                                   &s->performance_start_writes,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "performance-intr-w1c-writes",
                                   &s->performance_intr_w1c_writes,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "performance-evterr-reads",
                                   &s->performance_evterr_reads,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "performance-irq-update-calls",
                                   &s->performance_irq_update_calls,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "performance-irq-transitions",
                                   &s->performance_irq_transitions,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "performance-guest-read-ops",
                                   &s->performance_guest_read_ops,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "performance-guest-read-bytes",
                                   &s->performance_guest_read_bytes,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "performance-guest-write-ops",
                                   &s->performance_guest_write_ops,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "performance-guest-write-bytes",
                                   &s->performance_guest_write_bytes,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "cyclic-last-timer-virtual-ns",
                                   &s->cyclic_last_timer_virtual_ns,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_bool(obj, "audio-health-enable",
                             imx6_sdma_get_audio_health_enable,
                             imx6_sdma_set_audio_health_enable);
    for (unsigned int channel = 1;
         channel < IMX6_SDMA_CYCLIC_CHANNELS; channel++) {
        g_autofree char *completion_name =
            g_strdup_printf("performance-ch%u-completions", channel);
        g_autofree char *deferral_name =
            g_strdup_printf("performance-ch%u-deferrals", channel);
        g_autofree char *direction_name =
            g_strdup_printf("cyclic-ch%u-direction", channel);
        g_autofree char *peripheral_name =
            g_strdup_printf("cyclic-ch%u-peripheral", channel);
        g_autofree char *period_name =
            g_strdup_printf("cyclic-ch%u-period-bytes", channel);
        g_autofree char *late_name =
            g_strdup_printf("cyclic-ch%u-late-ns", channel);
        g_autofree char *resync_name =
            g_strdup_printf("cyclic-ch%u-deadline-resyncs", channel);
        g_autofree char *late_periods_name =
            g_strdup_printf("cyclic-ch%u-late-periods", channel);
        g_autofree char *max_late_name =
            g_strdup_printf("cyclic-ch%u-max-late-ns", channel);
        g_autofree char *multi_name =
            g_strdup_printf("cyclic-ch%u-multi-completion-callbacks", channel);
        g_autofree char *max_completed_name =
            g_strdup_printf("cyclic-ch%u-max-completions-per-callback", channel);

        object_property_add_uint64_ptr(obj, completion_name,
                                       &s->cyclic_completions[channel],
                                       OBJ_PROP_FLAG_READ);
        object_property_add_uint64_ptr(obj, deferral_name,
                                       &s->cyclic_deferrals[channel],
                                       OBJ_PROP_FLAG_READ);
        /* Read-only timing/route state for host-audio diagnosis. */
        object_property_add_uint8_ptr(obj, direction_name,
                                      &s->cyclic_audio_direction[channel],
                                      OBJ_PROP_FLAG_READ);
        object_property_add_uint32_ptr(obj, peripheral_name,
                                       &s->cyclic_audio_peripheral[channel],
                                       OBJ_PROP_FLAG_READ);
        object_property_add_uint32_ptr(obj, period_name,
                                       &s->cyclic_period_bytes[channel],
                                       OBJ_PROP_FLAG_READ);
        object_property_add_uint64_ptr(obj, late_name,
                                       &s->cyclic_late_ns[channel],
                                       OBJ_PROP_FLAG_READ);
        object_property_add_uint64_ptr(obj, resync_name,
                                       &s->cyclic_deadline_resyncs[channel],
                                       OBJ_PROP_FLAG_READ);
        object_property_add_uint64_ptr(obj, late_periods_name,
                                       &s->cyclic_late_periods[channel],
                                       OBJ_PROP_FLAG_READ);
        object_property_add_uint64_ptr(obj, max_late_name,
                                       &s->cyclic_max_late_ns[channel],
                                       OBJ_PROP_FLAG_READ);
        object_property_add_uint64_ptr(obj, multi_name,
                                       &s->cyclic_multi_completion_callbacks[channel],
                                       OBJ_PROP_FLAG_READ);
        object_property_add_uint8_ptr(obj, max_completed_name,
                                      &s->cyclic_max_completions_per_callback[channel],
                                      OBJ_PROP_FLAG_READ);
    }
}

static int imx6_sdma_post_load(void *opaque, int version_id)
{
    IMX6SDMAState *s = opaque;
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    unsigned int channel;

    /* Pre-v7 snapshots had no independent deadline state. */
    if (version_id < 7) {
        for (channel = 1; channel < IMX6_SDMA_CYCLIC_CHANNELS; channel++) {
            if (s->cyclic_active[channel] &&
                imx6_sdma_valid_period_bytes(s->cyclic_period_bytes[channel])) {
                s->cyclic_next_due_ns[channel] = now +
                    imx6_sdma_period_ns(s->cyclic_period_bytes[channel]);
            }
        }
    }
    if (imx6_sdma_any_cyclic_active(s)) {
        imx6_sdma_schedule_period(s);
    }
    return 0;
}

static const VMStateDescription vmstate_imx6_sdma = {
    .name = TYPE_IMX6_SDMA,
    .version_id = 7,
    .minimum_version_id = 1,
    .post_load = imx6_sdma_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX6SDMAState,
                             IMX6_SDMA_SIZE / sizeof(uint32_t)),
        VMSTATE_UINT32(intr_status, IMX6SDMAState),
        VMSTATE_UINT32(rejected_starts, IMX6SDMAState),
        VMSTATE_UINT8_ARRAY(cyclic_active, IMX6SDMAState,
                            IMX6_SDMA_CYCLIC_CHANNELS),
        VMSTATE_UINT8_ARRAY(cyclic_bd_count, IMX6SDMAState,
                            IMX6_SDMA_CYCLIC_CHANNELS),
        VMSTATE_UINT8_ARRAY(cyclic_next, IMX6SDMAState,
                            IMX6_SDMA_CYCLIC_CHANNELS),
        VMSTATE_UINT32_ARRAY(cyclic_bd_base, IMX6SDMAState,
                             IMX6_SDMA_CYCLIC_CHANNELS),
        VMSTATE_UINT32_ARRAY_V(cyclic_period_bytes, IMX6SDMAState,
                               IMX6_SDMA_CYCLIC_CHANNELS, 6),
        VMSTATE_UINT64_ARRAY_V(cyclic_next_due_ns, IMX6SDMAState,
                               IMX6_SDMA_CYCLIC_CHANNELS, 7),
        VMSTATE_UINT64_ARRAY_V(cyclic_late_ns, IMX6SDMAState,
                               IMX6_SDMA_CYCLIC_CHANNELS, 7),
        VMSTATE_UINT64_ARRAY_V(cyclic_deadline_resyncs, IMX6SDMAState,
                               IMX6_SDMA_CYCLIC_CHANNELS, 7),
        VMSTATE_UINT8_ARRAY(cyclic_audio_direction, IMX6SDMAState,
                            IMX6_SDMA_CYCLIC_CHANNELS),
        VMSTATE_UINT32_ARRAY(cyclic_audio_peripheral, IMX6SDMAState,
                             IMX6_SDMA_CYCLIC_CHANNELS),
        VMSTATE_UINT64_ARRAY(cyclic_completions, IMX6SDMAState,
                             IMX6_SDMA_CYCLIC_CHANNELS),
        VMSTATE_UINT64_ARRAY(cyclic_deferrals, IMX6SDMAState,
                             IMX6_SDMA_CYCLIC_CHANNELS),
        VMSTATE_UINT64_ARRAY(cyclic_capture_bytes, IMX6SDMAState,
                             IMX6_SDMA_CYCLIC_CHANNELS),
        VMSTATE_TIMER_PTR(period_timer, IMX6SDMAState),
        VMSTATE_END_OF_LIST(),
    },
};

static void imx6_sdma_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, imx6_sdma_reset);
    dc->vmsd = &vmstate_imx6_sdma;
    dc->desc = "i.MX6 SDMA command-channel register block";
}

static const TypeInfo imx6_sdma_type_info = {
    .name = TYPE_IMX6_SDMA,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMX6SDMAState),
    .instance_init = imx6_sdma_init,
    .class_init = imx6_sdma_class_init,
};

static void imx6_sdma_register_types(void)
{
    type_register_static(&imx6_sdma_type_info);
}

type_init(imx6_sdma_register_types)
