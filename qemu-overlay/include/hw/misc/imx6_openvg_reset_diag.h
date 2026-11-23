/*
 * Opt-in, source-bounded OpenVG reset diagnostic.
 *
 * This device deliberately implements only the reset/idle contract observed
 * in Pioneer's GC350 OpenVG driver.  It supplies neither a chip identity nor
 * command execution, DMA, or interrupts.
 */
#ifndef HW_MISC_IMX6_OPENVG_RESET_DIAG_H
#define HW_MISC_IMX6_OPENVG_RESET_DIAG_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMX6_OPENVG_RESET_DIAG "imx6-openvg-reset-diag"
OBJECT_DECLARE_SIMPLE_TYPE(IMX6OpenVGResetDiagState, IMX6_OPENVG_RESET_DIAG)

#define IMX6_OPENVG_RESET_DIAG_SIZE 0x4000

struct IMX6OpenVGResetDiagState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t clock_control;
    uint8_t reset_step;
    bool reset_sequence_valid;
    bool reset_complete;
};

#endif /* HW_MISC_IMX6_OPENVG_RESET_DIAG_H */
