/*
 * Minimal i.MX6 APBH DMA transport model for XDJ-XZ U-Boot.
 *
 * This covers only the channel-zero GPMI descriptor handoff observed in the
 * XZ firmware.  It is deliberately not a NAND, BCH, or generic DMA model.
 */
#ifndef HW_MISC_IMX6_APBH_DMA_H
#define HW_MISC_IMX6_APBH_DMA_H

#include "hw/core/sysbus.h"
#include "hw/misc/imx6_bch.h"
#include "hw/misc/imx6_gpmi_reset.h"
#include "qom/object.h"
#include "qemu/timer.h"

#define TYPE_IMX6_APBH_DMA "imx6-apbh-dma"
OBJECT_DECLARE_SIMPLE_TYPE(IMX6APBHDMAState, IMX6_APBH_DMA)

#define IMX6_APBH_DMA_SIZE 0x2000

struct IMX6APBHDMAState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[IMX6_APBH_DMA_SIZE / sizeof(uint32_t)];
    uint8_t ch0_phore;
    IMX6GPMIResetState *gpmi;
    bool transport_stopped;
    /*
     * The original Linux gpmi_send_command() submits a PIO-only CCW and an
     * immediately chained DMA_READ CCW.  Keep the authorization for that
     * second CCW explicit rather than inferring it from stale GPMI registers.
     */
    uint8_t pending_kind;
    uint32_t pending_next_descriptor;
    uint32_t pending_ctrl0;
    uint32_t pending_xfer_count;
    uint8_t pending_cs;
    uint8_t pending_address;
    QEMUTimer *wait_timer;
    bool wait_pending;
    uint32_t wait_descriptor;
    IMX6BCHState *bch;
    QEMUTimer *bch_timer;
    bool bch_pending;
    uint32_t bch_status0;
};

void imx6_apbh_dma_set_gpmi(IMX6APBHDMAState *s, IMX6GPMIResetState *gpmi);
void imx6_apbh_dma_set_bch(IMX6APBHDMAState *s, IMX6BCHState *bch);

#endif
