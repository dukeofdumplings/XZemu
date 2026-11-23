/*
 * Opt-in i.MX6 IPUv3 IPU1 trace/state window for XDJ-XZ diagnostics.
 *
 * This is deliberately not an IPUv3 execution model.  It preserves guest
 * register state for source-observed common/IDMAC accesses and reports those
 * accesses, but it has no scan timer, EOF production, DMA, or IRQ output.
 */
#ifndef HW_MISC_IMX6_IPU1_TRACE_H
#define HW_MISC_IMX6_IPU1_TRACE_H

#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "qom/object.h"
#include "qemu/timer.h"

#define TYPE_IMX6_IPU1_TRACE "imx6-ipu1-trace"
OBJECT_DECLARE_SIMPLE_TYPE(IMX6IPU1TraceState, IMX6_IPU1_TRACE)

#define IMX6_IPU1_TRACE_SIZE UINT64_C(0x400000)
#define IMX6_IPU1_TRACE_WORDS (IMX6_IPU1_TRACE_SIZE / sizeof(uint32_t))

/* IPUv3H: IPU base + 0x00200000 common window + 0x00100000 CPMEM. */
#define IMX6_IPU1_CPMEM_OFFSET UINT64_C(0x00300000)

/*
 * A deliberately narrow representation of the source-observed MEM_BG_SYNC
 * channel.  It is a read-only view of CPMEM and the IDMAC current-buffer
 * bit; it does not perform IPU DMA or alter channel state.
 */
typedef struct IMX6IPU1BG23Scanout {
    hwaddr address;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint8_t buffer;
} IMX6IPU1BG23Scanout;

struct IMX6IPU1TraceState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t *words;
    uint32_t trace_events;
    bool trace_suppressed;
    bool diagnostic_enable;
    /* B1 is deliberately independent and off unless explicitly requested. */
    bool bg23_eof_diagnostic_enable;
    qemu_irq bg23_irq;
    QEMUTimer *bg23_frame_timer;
    uint64_t bg23_generation;
    uint64_t bg23_scheduled_generation;
    uint8_t bg23_scheduled_buffer;
    uint8_t bg23_stage;
    bool bg23_scan_active_last;
};

bool imx6_ipu1_trace_bg23_scanout_get(const IMX6IPU1TraceState *s,
                                      IMX6IPU1BG23Scanout *scanout);

#endif /* HW_MISC_IMX6_IPU1_TRACE_H */
