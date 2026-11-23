/*
 * i.MX6 SSI configuration register model for the XDJ-XZ.
 *
 * Pioneer Linux programs SSI_SCR/SIER/STCR/SRCR, clock configuration and
 * FIFO watermarks around its cyclic SDMA setup.  The driver uses DMA mode,
 * so no SSI interrupt is asserted here.  Readable configuration and stable
 * empty TX FIFO status are enough for that hardware contract; serial audio
 * transport belongs to a later, separately evidenced gate.
 */
#include "qemu/osdep.h"
#include "hw/misc/imx6_ssi.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/visitor.h"
#include "hw/core/qdev-properties.h"
#include "system/memory.h"

#define SSI_SCR   0x10
#define SSI_SISR  0x14
#define SSI_SIER  0x18
#define SSI_STX0  0x00
#define SSI_STX1  0x04
#define SSI_SRX0  0x08
#define SSI_SRX1  0x0c

#define SSI_SCR_SSIEN (1U << 0)
#define SSI_SCR_TE    (1U << 1)
#define SSI_SCR_RE    (1U << 2)
#define SSI_SIER_TDMAE (1U << 20)
#define SSI_SIER_RDMAE (1U << 22)
#define SSI_SISR_TDE1 (1U << 13)
#define SSI_SISR_TDE0 (1U << 12)
#define SSI_SISR_TFE1 (1U << 1)
#define SSI_SISR_TFE0 (1U << 0)

static void imx6_ssi_drain_audio(IMX6SSIState *s, size_t available)
{
    while (s->audio_out && s->audio_fifo && s->audio_fifo->len && available) {
        size_t offered = MIN((size_t)s->audio_fifo->len, available);
        size_t written = audio_be_write(s->audio_be, s->audio_out,
                                        s->audio_fifo->data, offered);

        if (written > offered) {
            written = offered;
        }
        s->host_audio_bytes += written;
        if (written) {
            g_byte_array_remove_range(s->audio_fifo, 0, written);
            available -= written;
            if (s->host_audio_health_enable && !s->audio_fifo->len) {
                s->host_audio_oldest_enqueue_host_ns = 0;
            }
        }
        if (written != offered) {
            s->host_audio_short_writes++;
            s->host_audio_short_write_bytes += offered - written;
            break;
        }
    }
}

static void imx6_ssi_sample_mix_health(IMX6SSIState *s)
{
    AudioOutputHealth health;

    if (!s->host_audio_health_enable) {
        return;
    }
    if (!audio_be_get_output_health(s->audio_out, &health)) {
        return;
    }
    s->host_audio_mix_source_frames = health.source_mixed_frames;
    s->host_audio_mix_min_frames = health.mixer_min_frames;
    s->host_audio_mix_active_voices = health.active_voices;
    if (health.source_is_min) {
        s->host_audio_mix_min_hits++;
    }
    if (health.live_zero && health.active_voices > 1) {
        s->host_audio_mix_live_zero_events++;
    }
}

static void imx6_ssi_set_audio_active(IMX6SSIState *s, bool active)
{
    if (!s->audio_out || s->host_audio_active == active) {
        return;
    }

    audio_be_set_active_out(s->audio_be, s->audio_out, active);
    s->host_audio_active = active;
}

bool imx6_ssi_dma_request_enabled(const IMX6SSIState *s, bool transmit)
{
    uint32_t scr = s->regs[SSI_SCR / sizeof(uint32_t)];
    uint32_t sier = s->regs[SSI_SIER / sizeof(uint32_t)];

    if (!(scr & SSI_SCR_SSIEN)) {
        return false;
    }
    if (transmit) {
        return (scr & SSI_SCR_TE) && (sier & SSI_SIER_TDMAE);
    }
    return (scr & SSI_SCR_RE) && (sier & SSI_SIER_RDMAE);
}

void imx6_ssi_dma_tx(IMX6SSIState *s, const uint8_t *data, size_t length)
{
    size_t accepted;
    size_t prior_length;

    if (length >= sizeof(uint32_t)) {
        s->regs[SSI_STX0 / sizeof(uint32_t)] =
            ldl_le_p(data + length - sizeof(uint32_t));
    }
    s->dma_tx_bytes += length;
    if (!s->audio_out || !s->audio_fifo) {
        return;
    }

    /*
     * Pioneer ALSA keeps valid cyclic SSI TX descriptors running while RBP
     * is on the standby screen.  The captured playback buffers are all zero
     * there.  With lazy host playback selected, dropping that silence before
     * the CoreAudio voice is activated avoids a host queue/callback workload
     * without altering the guest's SSI/SDMA contract.  Once a real sample is
     * present, activate the sink and retain subsequent silent frames too so
     * playback timing is continuous.
     */
    if (s->host_audio_lazy_activate && !s->host_audio_active &&
        !imx6_ssi_pcm_has_signal(data, length, s->host_audio_s24le)) {
        return;
    }

    accepted = MIN(length, IMX6_SSI_AUDIO_FIFO_BYTES - s->audio_fifo->len);
    /* A CoreAudio S32 frame cannot contain a fractional source slot. */
    accepted &= ~(sizeof(uint32_t) - 1);
    if (accepted) {
        if (s->host_audio_health_enable && !s->audio_fifo->len) {
            s->host_audio_oldest_enqueue_host_ns =
                qemu_clock_get_ns(QEMU_CLOCK_HOST);
        }
        if (s->host_audio_s24le) {
            prior_length = s->audio_fifo->len;
            g_byte_array_set_size(s->audio_fifo, prior_length + accepted);
            imx6_ssi_convert_s24le_to_s32le(s->audio_fifo->data +
                                             prior_length, data, accepted);
        } else {
            g_byte_array_append(s->audio_fifo, data, accepted);
        }
        s->host_audio_submitted_bytes += accepted;
        s->host_audio_fifo_peak_bytes = MAX(s->host_audio_fifo_peak_bytes,
                                            s->audio_fifo->len);
    }
    if (accepted != length) {
        if (!s->host_audio_drop_bytes) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "imx6-ssi: host PCM FIFO overflow; preserving queued audio\n");
        }
        s->host_audio_drop_bytes += length - accepted;
    }
    if (accepted && s->host_audio_lazy_activate && !s->host_audio_active) {
        imx6_ssi_set_audio_active(s, true);
    }
    /*
     * SDMA completion owns guest BD/IRQ timing.  The optional host sink must
     * never make that virtual timer wait on CoreAudio.  In callback-only mode
     * the backend consumes the queued bytes from imx6_ssi_audio_out_cb().
     */
    if (!s->host_audio_callback_only) {
        imx6_ssi_drain_audio(s, s->audio_fifo->len);
    }
    imx6_ssi_sample_mix_health(s);
}

void imx6_ssi_dma_rx(IMX6SSIState *s, uint8_t *data, size_t length)
{
    memset(data, 0, length);
    s->dma_rx_bytes += length;
}

static uint64_t imx6_ssi_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX6SSIState *s = opaque;

    switch (offset) {
    case SSI_SISR:
        /* No serial source is attached: transmit FIFOs remain empty. */
        return s->regs[SSI_SCR / sizeof(uint32_t)] & SSI_SCR_SSIEN ?
            SSI_SISR_TDE1 | SSI_SISR_TDE0 | SSI_SISR_TFE1 | SSI_SISR_TFE0 : 0;
    case SSI_SRX0:
    case SSI_SRX1:
        /* Explicit silent receive source until real capture is modeled. */
        return 0;
    default:
        return s->regs[offset / sizeof(uint32_t)];
    }
}

static void imx6_ssi_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    IMX6SSIState *s = opaque;

    if (offset == SSI_SISR || offset == SSI_SRX0 || offset == SSI_SRX1) {
        return;
    }
    s->regs[offset / sizeof(uint32_t)] = value;
}

static const MemoryRegionOps imx6_ssi_ops = {
    .read = imx6_ssi_read,
    .write = imx6_ssi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void imx6_ssi_reset(DeviceState *dev)
{
    IMX6SSIState *s = IMX6_SSI(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->dma_tx_bytes = 0;
    s->dma_rx_bytes = 0;
    s->host_audio_bytes = 0;
    s->host_audio_submitted_bytes = 0;
    s->host_audio_short_write_bytes = 0;
    s->host_audio_drop_bytes = 0;
    s->host_audio_callback_count = 0;
    s->host_audio_oldest_enqueue_host_ns = 0;
    s->host_audio_queue_max_age_ns = 0;
    s->host_audio_mix_min_hits = 0;
    s->host_audio_mix_live_zero_events = 0;
    s->host_audio_mix_source_frames = 0;
    s->host_audio_mix_min_frames = 0;
    s->host_audio_mix_active_voices = 0;
    s->host_audio_short_writes = 0;
    s->host_audio_fifo_peak_bytes = 0;
    if (s->audio_fifo) {
        g_byte_array_set_size(s->audio_fifo, 0);
    }
    if (s->audio_out) {
        imx6_ssi_set_audio_active(s, !s->host_audio_lazy_activate);
    }
}

static void imx6_ssi_audio_out_cb(void *opaque, int available)
{
    IMX6SSIState *s = opaque;

    s->host_audio_callback_count++;
    if (available > 0) {
        imx6_ssi_drain_audio(s, available);
    }
    imx6_ssi_sample_mix_health(s);
}

static void imx6_ssi_get_audio_queue_age_ns(Object *obj, Visitor *v,
                                              const char *name, void *opaque,
                                              Error **errp)
{
    IMX6SSIState *s = IMX6_SSI(obj);
    uint64_t value = 0;

    if (s->host_audio_health_enable && s->audio_fifo && s->audio_fifo->len &&
        s->host_audio_oldest_enqueue_host_ns) {
        uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_HOST);

        value = now - s->host_audio_oldest_enqueue_host_ns;
        s->host_audio_queue_max_age_ns = MAX(s->host_audio_queue_max_age_ns,
                                             value);
    }
    visit_type_uint64(v, name, &value, errp);
}

static void imx6_ssi_get_audio_fifo_bytes(Object *obj, Visitor *v,
                                           const char *name, void *opaque,
                                           Error **errp)
{
    IMX6SSIState *s = IMX6_SSI(obj);
    uint64_t value = s->audio_fifo ? s->audio_fifo->len : 0;

    visit_type_uint64(v, name, &value, errp);
}

static void imx6_ssi_get_virtual_time_ns(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    uint64_t value = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    visit_type_uint64(v, name, &value, errp);
}

static bool imx6_ssi_get_audio_health_enable(Object *obj, Error **errp)
{
    return IMX6_SSI(obj)->host_audio_health_enable;
}

static void imx6_ssi_set_audio_health_enable(Object *obj, bool value,
                                              Error **errp)
{
    IMX6_SSI(obj)->host_audio_health_enable = value;
}

static void imx6_ssi_realize(DeviceState *dev, Error **errp)
{
    IMX6SSIState *s = IMX6_SSI(dev);
    struct audsettings settings = {
        .freq = 44100,
        .nchannels = 2,
        .fmt = AUDIO_FORMAT_S32,
        .big_endian = false,
    };

    if (!s->host_audio_enable) {
        return;
    }
    if (!audio_be_check(&s->audio_be, errp)) {
        return;
    }
    s->audio_fifo = g_byte_array_sized_new(IMX6_SSI_AUDIO_FIFO_BYTES);
    s->audio_out = audio_be_open_out(s->audio_be, NULL, TYPE_IMX6_SSI,
                                     s, imx6_ssi_audio_out_cb, &settings);
    imx6_ssi_set_audio_active(s, !s->host_audio_lazy_activate);
}

static void imx6_ssi_unrealize(DeviceState *dev)
{
    IMX6SSIState *s = IMX6_SSI(dev);

    audio_be_close_out(s->audio_be, s->audio_out);
    s->audio_out = NULL;
    s->host_audio_active = false;
    g_clear_pointer(&s->audio_fifo, g_byte_array_unref);
}

static void imx6_ssi_init(Object *obj)
{
    IMX6SSIState *s = IMX6_SSI(obj);

    memory_region_init_io(&s->iomem, obj, &imx6_ssi_ops, s,
                          TYPE_IMX6_SSI, IMX6_SSI_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    /* Read-only host-sink accounting for crackle/underrun diagnosis. */
    object_property_add_uint64_ptr(obj, "host-audio-submitted-bytes",
                                   &s->host_audio_submitted_bytes,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "host-audio-delivered-bytes",
                                   &s->host_audio_bytes,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "host-audio-short-write-bytes",
                                   &s->host_audio_short_write_bytes,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "host-audio-dropped-bytes",
                                   &s->host_audio_drop_bytes,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "host-audio-callbacks",
                                   &s->host_audio_callback_count,
                                   OBJ_PROP_FLAG_READ);
    object_property_add(obj, "host-audio-fifo-bytes", "uint64",
                        imx6_ssi_get_audio_fifo_bytes, NULL, NULL, NULL);
    object_property_add(obj, "host-audio-queue-age-ns", "uint64",
                        imx6_ssi_get_audio_queue_age_ns, NULL, NULL, NULL);
    object_property_add(obj, "host-audio-virtual-time-ns", "uint64",
                        imx6_ssi_get_virtual_time_ns, NULL, NULL, NULL);
    object_property_add_uint64_ptr(obj, "host-audio-queue-max-age-ns",
                                   &s->host_audio_queue_max_age_ns,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "host-audio-mix-min-hits",
                                   &s->host_audio_mix_min_hits,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "host-audio-mix-live-zero-events",
                                   &s->host_audio_mix_live_zero_events,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "host-audio-mix-source-frames",
                                   &s->host_audio_mix_source_frames,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "host-audio-mix-min-frames",
                                   &s->host_audio_mix_min_frames,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "host-audio-mix-active-voices",
                                   &s->host_audio_mix_active_voices,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "host-audio-fifo-peak-bytes",
                                   &s->host_audio_fifo_peak_bytes,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_bool(obj, "host-audio-health-enable",
                             imx6_ssi_get_audio_health_enable,
                             imx6_ssi_set_audio_health_enable);
}

static const VMStateDescription vmstate_imx6_ssi = {
    .name = TYPE_IMX6_SSI,
    .version_id = 3,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX6SSIState,
                             IMX6_SSI_SIZE / sizeof(uint32_t)),
        VMSTATE_UINT64(dma_tx_bytes, IMX6SSIState),
        VMSTATE_UINT64(dma_rx_bytes, IMX6SSIState),
        VMSTATE_UINT64_V(host_audio_bytes, IMX6SSIState, 3),
        VMSTATE_END_OF_LIST(),
    },
};

static const Property imx6_ssi_props[] = {
    DEFINE_PROP_BOOL("host-audio-enable", IMX6SSIState,
                     host_audio_enable, false),
    /* The XDJ host-audio path is sourced as S24_LE in 32-bit slots. */
    DEFINE_PROP_BOOL("host-audio-s24le", IMX6SSIState,
                     host_audio_s24le, true),
    DEFINE_PROP_BOOL("host-audio-callback-only", IMX6SSIState,
                     host_audio_callback_only, false),
    DEFINE_PROP_BOOL("host-audio-lazy-activate", IMX6SSIState,
                     host_audio_lazy_activate, false),
};

static void imx6_ssi_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, imx6_ssi_reset);
    dc->realize = imx6_ssi_realize;
    dc->unrealize = imx6_ssi_unrealize;
    dc->vmsd = &vmstate_imx6_ssi;
    dc->desc = "i.MX6 SSI register block";

    device_class_set_props(dc, imx6_ssi_props);
}

static const TypeInfo imx6_ssi_type_info = {
    .name = TYPE_IMX6_SSI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMX6SSIState),
    .instance_init = imx6_ssi_init,
    .class_init = imx6_ssi_class_init,
};

static void imx6_ssi_register_types(void)
{
    type_register_static(&imx6_ssi_type_info);
}

type_init(imx6_ssi_register_types)
