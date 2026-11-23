/*
 * Minimal, opt-in diagnostic model for the i.MX6 3D Vivante window.
 *
 * This does not emulate a GC2000 renderer or identify a particular XDJ-XZ
 * GPU revision.  It models only the reset/idle handshake exercised by the
 * shipped Pioneer Galcore 4.6.9.9754 driver's _ResetGPU().
 */
#ifndef HW_MISC_IMX6_GC2000_DIAG_H
#define HW_MISC_IMX6_GC2000_DIAG_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMX6_GC2000_DIAG "imx6-gc2000-diag"
OBJECT_DECLARE_SIMPLE_TYPE(IMX6GC2000DiagState, IMX6_GC2000_DIAG)

#define IMX6_GC2000_DIAG_SIZE 0x4000

struct IMX6GC2000DiagState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t clock_control;
    uint32_t power_controls;
    uint32_t pulse_eater;
    uint32_t mmu_fe_page_table;
    uint32_t mmu_ra_page_table;
    uint32_t mmu_tx_page_table;
    uint32_t mmu_pe_page_table;
    uint32_t mmu_pez_page_table;
    uint32_t event_enable;
    uint32_t fe_address;
    uint32_t fe_control;
    uint8_t programming_step;
    bool memory_counter_reset_armed;
    bool memory_counters_reset_complete;
    bool power_controls_disabled;
    bool pulse_eater_asserted;
    bool pulse_eater_pulsed;
    bool isolate_seen;
    bool soft_reset_seen;
    bool reset_complete;
    bool programming_order_valid;
    bool fe_start_shape_valid;
    /*
     * Explicit diagnostic profile, not an XDJ-XZ hardware measurement.
     * Its model/revision come from an independently observed i.MX6Q GC2000.
     */
    bool diagnostic_reference_identity;
};

#endif /* HW_MISC_IMX6_GC2000_DIAG_H */
