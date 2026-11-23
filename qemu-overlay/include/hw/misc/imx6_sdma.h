/* Minimal i.MX6 SDMA command-channel register model for the XDJ-XZ. */
#ifndef HW_MISC_IMX6_SDMA_H
#define HW_MISC_IMX6_SDMA_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"

typedef struct IMX6SSIState IMX6SSIState;

#define TYPE_IMX6_SDMA "imx6-sdma"
OBJECT_DECLARE_SIMPLE_TYPE(IMX6SDMAState, IMX6_SDMA)

#define IMX6_SDMA_SIZE 0x1000
#define IMX6_SDMA_CYCLIC_CHANNELS 5
#define IMX6_SDMA_MAX_CYCLIC_DESCRIPTORS 32

struct IMX6SDMAState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t regs[IMX6_SDMA_SIZE / sizeof(uint32_t)];
    uint32_t intr_status;
    uint32_t rejected_starts;
    bool cyclic_diagnostic_enable;
    bool cyclic_audio_enable;
    /* Bounded, source-identified SSI playback deadline catch-up. */
    bool cyclic_audio_catchup_enable;
    bool cyclic_static_metadata_fastpath;
    /* Aggregate audio timing counters are off unless explicitly enabled. */
    bool audio_health_enable;
    /* Opt-in aggregate profiling; never emitted from the period hot path. */
    bool performance_diagnostic_enable;
    bool performance_irq_level;
    uint64_t performance_period_callbacks;
    uint64_t performance_start_writes;
    uint64_t performance_intr_w1c_writes;
    uint64_t performance_evterr_reads;
    uint64_t performance_irq_update_calls;
    uint64_t performance_irq_transitions;
    uint64_t performance_guest_read_ops;
    uint64_t performance_guest_read_bytes;
    uint64_t performance_guest_write_ops;
    uint64_t performance_guest_write_bytes;
    uint8_t cyclic_active[IMX6_SDMA_CYCLIC_CHANNELS];
    uint8_t cyclic_bd_count[IMX6_SDMA_CYCLIC_CHANNELS];
    uint8_t cyclic_next[IMX6_SDMA_CYCLIC_CHANNELS];
    bool cyclic_static_metadata[IMX6_SDMA_CYCLIC_CHANNELS];
    uint32_t cyclic_bd_base[IMX6_SDMA_CYCLIC_CHANNELS];
    uint32_t cyclic_bd_mode_template[IMX6_SDMA_CYCLIC_CHANNELS]
                                     [IMX6_SDMA_MAX_CYCLIC_DESCRIPTORS];
    uint32_t cyclic_bd_buffer_addr[IMX6_SDMA_CYCLIC_CHANNELS]
                                   [IMX6_SDMA_MAX_CYCLIC_DESCRIPTORS];
    uint32_t cyclic_period_bytes[IMX6_SDMA_CYCLIC_CHANNELS];
    uint64_t cyclic_next_due_ns[IMX6_SDMA_CYCLIC_CHANNELS];
    uint64_t cyclic_late_ns[IMX6_SDMA_CYCLIC_CHANNELS];
    uint64_t cyclic_deadline_resyncs[IMX6_SDMA_CYCLIC_CHANNELS];
    uint64_t cyclic_late_periods[IMX6_SDMA_CYCLIC_CHANNELS];
    uint64_t cyclic_max_late_ns[IMX6_SDMA_CYCLIC_CHANNELS];
    uint64_t cyclic_multi_completion_callbacks[IMX6_SDMA_CYCLIC_CHANNELS];
    uint8_t cyclic_max_completions_per_callback[IMX6_SDMA_CYCLIC_CHANNELS];
    uint8_t cyclic_audio_direction[IMX6_SDMA_CYCLIC_CHANNELS];
    uint32_t cyclic_audio_peripheral[IMX6_SDMA_CYCLIC_CHANNELS];
    uint64_t cyclic_completions[IMX6_SDMA_CYCLIC_CHANNELS];
    uint64_t cyclic_deferrals[IMX6_SDMA_CYCLIC_CHANNELS];
    uint64_t cyclic_capture_bytes[IMX6_SDMA_CYCLIC_CHANNELS];
    uint64_t cyclic_last_timer_virtual_ns;
    QEMUTimer *period_timer;
    qemu_irq irq;
    IMX6SSIState *ssi[2];
};

#endif
