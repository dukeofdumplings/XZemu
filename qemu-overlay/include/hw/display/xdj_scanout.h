/*
 * XDJ-XZ diagnostic framebuffer scanout.
 *
 * This is deliberately not an IPUv3 implementation.  Until the Pioneer
 * guest's IPU/IDMAC programming is observed, it exposes only the measured
 * physical fb0 surface when the parent SoC opt-in property is enabled.
 */
#ifndef HW_DISPLAY_XDJ_SCANOUT_H
#define HW_DISPLAY_XDJ_SCANOUT_H

#include "hw/core/sysbus.h"
#include "hw/display/xdj_scanout_rgb565.h"
#include "hw/misc/imx6_ipu1_trace.h"
#include "qemu/timer.h"
#include "ui/console.h"

#define TYPE_IMX6_XDJ_SCANOUT "imx6-xdj-scanout"
OBJECT_DECLARE_SIMPLE_TYPE(IMX6XDJScanoutState, IMX6_XDJ_SCANOUT)

#define XDJ_SCANOUT_DIAG_WIDTH       800u
#define XDJ_SCANOUT_DIAG_HEIGHT      480u
#define XDJ_SCANOUT_DIAG_PITCH       1600u
#define XDJ_SCANOUT_DIAG_GUEST_PHYS  0x18000000u

struct IMX6XDJScanoutState {
    SysBusDevice parent_obj;

    QemuConsole *con;
    QEMUTimer *refresh_timer;
    bool diagnostic_enable;
    /* Separate opt-in: resolve source pixels from BG23 CPMEM/IDMAC state. */
    bool bg23_cpmem_diagnostic_enable;
    IMX6IPU1TraceState *ipu1_trace;
    bool bg23_source_valid;
    hwaddr bg23_source_address;
    uint8_t bg23_source_buffer;
    bool invalidate;
    /* Exact last guest frame; suppresses redundant host-only redraws. */
    uint8_t *last_frame;
    /* Opt-in full-frame RAM read, with original row path retained. */
    bool bulk_scanout_read;
    uint8_t *bulk_frame;
    bool last_frame_valid;
};

#endif /* HW_DISPLAY_XDJ_SCANOUT_H */
