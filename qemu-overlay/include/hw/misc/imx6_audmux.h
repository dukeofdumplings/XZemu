/* Minimal i.MX6 AUDMUX v2 register block. */
#ifndef HW_MISC_IMX6_AUDMUX_H
#define HW_MISC_IMX6_AUDMUX_H

#include "hw/core/sysbus.h"

#define TYPE_IMX6_AUDMUX "imx6-audmux"
OBJECT_DECLARE_SIMPLE_TYPE(IMX6AUDMUXState, IMX6_AUDMUX)

#define IMX6_AUDMUX_SIZE 0x1000
#define IMX6_AUDMUX_PORTS 8

struct IMX6AUDMUXState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t ptcr[IMX6_AUDMUX_PORTS];
    uint32_t pdcr[IMX6_AUDMUX_PORTS];
};

#endif /* HW_MISC_IMX6_AUDMUX_H */
