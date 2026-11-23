/*
 * XDJ-XZ partial functional IPUv3 BG23 CPMEM scanout diagnostic stub.
 *
 * The legacy diagnostic bridge displays the physical-unit fb0 capture:
 * 800x480 RGB565 at guest physical address 0x18000000, with a 1600-byte
 * pitch.  A separate opt-in path resolves that same constrained format from
 * IPUv3 CPMEM and BG23's IDMAC current-buffer state.
 *
 * IMPLEMENTED: guest-selected EBA0/EBA1 and current-buffer selection for the
 * observed 800x480 RGB565 / 1600-byte-stride BG23 contract; host conversion
 * to XRGB and a sampled QEMU graphical console refresh at 10 Hz.
 *
 * NOT IMPLEMENTED: IPU DMA transactions, DMFC arbitration, DC microcode, DI
 * signal timing, DP blending, foreground channel 27, other CPMEM formats,
 * Vivante rendering, or any guest-state mutation/interrupt production here.
 * BG23 EOF remains a separate opt-in functional approximation in
 * imx6_ipu1_trace.c.  This must not be mistaken for a general IPUv3 model.
 */
#include "qemu/osdep.h"
#include "hw/display/xdj_scanout.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/qdev-properties.h"
#include "system/address-spaces.h"

#define XDJ_SCANOUT_REFRESH_NS (NANOSECONDS_PER_SECOND / 10)

static void xdj_scanout_draw_rgb565(uint8_t *dst, const uint8_t *src)
{
    unsigned int i;

    for (i = 0; i < XDJ_SCANOUT_DIAG_WIDTH; i++) {
        /* The QEMU console surface is XRGB8888 for this diagnostic stub. */
        stl_le_p(dst, xdj_scanout_rgb565_to_xrgb8888(lduw_le_p(src)));
        src += sizeof(uint16_t);
        dst += sizeof(uint32_t);
    }
}

static bool xdj_scanout_update_display(void *opaque)
{
    IMX6XDJScanoutState *s = opaque;
    DisplaySurface *surface;
    uint8_t source[XDJ_SCANOUT_DIAG_PITCH];
    IMX6IPU1BG23Scanout bg23;
    hwaddr source_address = XDJ_SCANOUT_DIAG_GUEST_PHYS;
    unsigned int row;
    bool changed;

    if (!s->con) {
        return true;
    }

    surface = qemu_console_surface(s->con);
    if (surface_width(surface) != XDJ_SCANOUT_DIAG_WIDTH ||
        surface_height(surface) != XDJ_SCANOUT_DIAG_HEIGHT) {
        qemu_console_resize(s->con, XDJ_SCANOUT_DIAG_WIDTH,
                            XDJ_SCANOUT_DIAG_HEIGHT);
        surface = qemu_console_surface(s->con);
        s->invalidate = true;
    }

    /* The standard graphical console surface is 32-bit XRGB. */
    if (surface_bits_per_pixel(surface) != 32) {
        return true;
    }

    if (s->bg23_cpmem_diagnostic_enable) {
        if (!imx6_ipu1_trace_bg23_scanout_get(s->ipu1_trace, &bg23)) {
            s->bg23_source_valid = false;
            return true;
        }
        source_address = bg23.address;
        if (!s->bg23_source_valid || s->bg23_source_address != source_address ||
            s->bg23_source_buffer != bg23.buffer) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          TYPE_IMX6_XDJ_SCANOUT ": BG23_CPMEM source=0x%"
                          HWADDR_PRIx " buffer=%u %ux%u stride=%u\n",
                          source_address, bg23.buffer, bg23.width,
                          bg23.height, bg23.stride);
            s->bg23_source_valid = true;
            s->bg23_source_address = source_address;
            s->bg23_source_buffer = bg23.buffer;
        }
    }

    /*
     * Keep this independent from QEMU's global RAM dirty logging: on this
     * TCG-only machine, logging a 1 GiB RAM region slows guest execution more
     * than reading this 768 KiB diagnostic surface at 10 Hz.  Direct reads
     * also make writes that predate the first refresh visible.
     */
    changed = s->invalidate || !s->last_frame_valid;
    if (s->bulk_scanout_read) {
        /* Exactly one contiguous guest-RAM snapshot per refresh.  Keep this
         * on the QEMU device thread; never race a host display worker with
         * live guest RAM or bypass MMIO/memory transaction semantics. */
        size_t bytes = XDJ_SCANOUT_DIAG_PITCH * XDJ_SCANOUT_DIAG_HEIGHT;
        MemTxResult result = address_space_read(&address_space_memory,
                                                 source_address,
                                                 MEMTXATTRS_UNSPECIFIED,
                                                 s->bulk_frame, bytes);

        if (result != MEMTX_OK) {
            return true;
        }
        if (!changed && memcmp(s->last_frame, s->bulk_frame, bytes)) {
            changed = true;
        }
        memcpy(s->last_frame, s->bulk_frame, bytes);
    } else {
        /* Reference path retained for per-run A/B; exact same pixels. */
        for (row = 0; row < XDJ_SCANOUT_DIAG_HEIGHT; row++) {
            MemTxResult result;
            uint8_t *cached = s->last_frame + row * XDJ_SCANOUT_DIAG_PITCH;

            result = address_space_read(&address_space_memory,
                                        source_address +
                                        row * XDJ_SCANOUT_DIAG_PITCH,
                                        MEMTXATTRS_UNSPECIFIED, source,
                                        sizeof(source));
            if (result != MEMTX_OK) {
                return true;
            }
            if (!changed && memcmp(cached, source, sizeof(source))) {
                changed = true;
            }
            memcpy(cached, source, sizeof(source));
        }
    }
    s->last_frame_valid = true;
    if (!changed) {
        return true;
    }
    for (row = 0; row < XDJ_SCANOUT_DIAG_HEIGHT; row++) {
        uint8_t *destination = surface_data(surface) +
            row * surface_stride(surface);

        xdj_scanout_draw_rgb565(destination,
                                s->last_frame + row * XDJ_SCANOUT_DIAG_PITCH);
    }
    s->invalidate = false;
    qemu_console_update_full(s->con);
    return true;
}

static void xdj_scanout_invalidate_display(void *opaque)
{
    IMX6XDJScanoutState *s = opaque;

    s->invalidate = true;
    s->bg23_source_valid = false;
    s->last_frame_valid = false;
}

static const GraphicHwOps xdj_scanout_graphic_ops = {
    .invalidate = xdj_scanout_invalidate_display,
    .gfx_update = xdj_scanout_update_display,
};

static void xdj_scanout_refresh_cb(void *opaque)
{
    IMX6XDJScanoutState *s = opaque;

    xdj_scanout_update_display(s);
    timer_mod(s->refresh_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + XDJ_SCANOUT_REFRESH_NS);
}

static void xdj_scanout_reset(DeviceState *dev)
{
    IMX6XDJScanoutState *s = IMX6_XDJ_SCANOUT(dev);

    s->invalidate = true;
}

static void xdj_scanout_realize(DeviceState *dev, Error **errp)
{
    IMX6XDJScanoutState *s = IMX6_XDJ_SCANOUT(dev);

    if (!s->diagnostic_enable) {
        return;
    }

    s->con = qemu_graphic_console_create(dev, 0, &xdj_scanout_graphic_ops, s);
    s->last_frame = g_malloc(XDJ_SCANOUT_DIAG_PITCH * XDJ_SCANOUT_DIAG_HEIGHT);
    if (s->bulk_scanout_read) {
        s->bulk_frame = g_malloc(XDJ_SCANOUT_DIAG_PITCH *
                                 XDJ_SCANOUT_DIAG_HEIGHT);
    }
    s->invalidate = true;
    s->refresh_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                    xdj_scanout_refresh_cb, s);
    timer_mod(s->refresh_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + XDJ_SCANOUT_REFRESH_NS);
}

static void xdj_scanout_unrealize(DeviceState *dev)
{
    IMX6XDJScanoutState *s = IMX6_XDJ_SCANOUT(dev);

    if (s->refresh_timer) {
        timer_free(s->refresh_timer);
        s->refresh_timer = NULL;
    }
    if (s->con) {
        qemu_graphic_console_close(s->con);
        s->con = NULL;
    }
    g_clear_pointer(&s->last_frame, g_free);
    g_clear_pointer(&s->bulk_frame, g_free);
    s->last_frame_valid = false;
}

static const Property xdj_scanout_props[] = {
    DEFINE_PROP_BOOL("bulk-scanout-read", IMX6XDJScanoutState,
                     bulk_scanout_read, false),
};

static void xdj_scanout_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = xdj_scanout_realize;
    dc->unrealize = xdj_scanout_unrealize;
    device_class_set_legacy_reset(dc, xdj_scanout_reset);
    dc->desc = "XDJ-XZ partial functional IPUv3 BG23 CPMEM scanout diagnostic stub";
    device_class_set_props(dc, xdj_scanout_props);
    dc->user_creatable = false;
}

static const TypeInfo xdj_scanout_info = {
    .name = TYPE_IMX6_XDJ_SCANOUT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMX6XDJScanoutState),
    .class_init = xdj_scanout_class_init,
};

static void xdj_scanout_register_types(void)
{
    type_register_static(&xdj_scanout_info);
}

type_init(xdj_scanout_register_types)
