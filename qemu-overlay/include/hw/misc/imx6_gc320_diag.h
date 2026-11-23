/*
 * Opt-in diagnostic state for the i.MX6 GC320 2D Vivante window.
 *
 * This is not a GC320 renderer, MMU, command processor, or IRQ device.  It
 * exists only to model the source-observed identity, AXI limit, and reset/idle
 * handshake used by the Pioneer Galcore driver.
 */
#ifndef HW_MISC_IMX6_GC320_DIAG_H
#define HW_MISC_IMX6_GC320_DIAG_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMX6_GC320_DIAG "imx6-gc320-diag"
OBJECT_DECLARE_SIMPLE_TYPE(IMX6GC320DiagState, IMX6_GC320_DIAG)

#define IMX6_GC320_DIAG_SIZE 0x4000

struct IMX6GC320DiagState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t clock_control;
    uint32_t power_controls;
    uint32_t pulse_eater;
    uint32_t axi_outstanding;
    uint8_t reset_step;
    bool reset_in_progress;
    bool reset_sequence_valid;
    bool reset_complete;
    bool axi_read_observed;
    bool axi_low_byte_programmed;
    /* A provisional source-derived profile, never a physical-XZ claim. */
    bool diagnostic_gc320_model;
};

#endif /* HW_MISC_IMX6_GC320_DIAG_H */
