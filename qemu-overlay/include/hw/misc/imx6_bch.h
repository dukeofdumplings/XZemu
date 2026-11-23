/* Minimal guest-visible i.MX6 BCH register block. */
#ifndef HW_MISC_IMX6_BCH_H
#define HW_MISC_IMX6_BCH_H

#include "hw/core/sysbus.h"

typedef struct IMX6GPMIResetState IMX6GPMIResetState;

#define TYPE_IMX6_BCH "imx6-bch"
OBJECT_DECLARE_SIMPLE_TYPE(IMX6BCHState, IMX6_BCH)

#define IMX6_BCH_SIZE 0x4000

struct IMX6BCHState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[IMX6_BCH_SIZE / sizeof(uint32_t)];
    uint32_t ctrl_config;
    uint32_t status0;
    bool complete_irq;
    bool status0_read_logged;
    IMX6GPMIResetState *gpmi;
};

/* Completion is a BCH engine result; it is never guest-writable via CTRL. */
void imx6_bch_complete(IMX6BCHState *s, uint32_t status0);
bool imx6_bch_is_operational(const IMX6BCHState *s);
void imx6_bch_set_gpmi(IMX6BCHState *s, IMX6GPMIResetState *gpmi);

#endif
