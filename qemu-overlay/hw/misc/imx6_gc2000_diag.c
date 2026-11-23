/*
 * Opt-in diagnostic state machine for the i.MX6 3D Vivante reset path.
 *
 * Source contract: Pioneer XDJ-XZ Linux 3.0.101, Galcore 4.6.9.9754,
 * gc_hal_kernel_hardware.c:_ResetGPU().  The driver disables power controls,
 * pulses PM_PULSE_EATER bit 0, isolates the core, asserts/deasserts soft
 * reset, removes isolation, then polls FE/3D/2D idle state indefinitely.
 *
 * H4 additionally records the source-observed MMU page-table, event-enable,
 * and FE programming writes.  Recording a valid FE kick has no command,
 * DMA, memory, IRQ, or rendering effect.
 */
#include "qemu/osdep.h"
#include "hw/misc/imx6_gc2000_diag.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define HI_CLOCK_CONTROL 0x000
#define HI_IDLE_STATE     0x004
#define HI_CHIP_IDENTITY  0x018
#define HI_CHIP_FEATURE   0x01c
#define HI_CHIP_MODEL     0x020
#define HI_CHIP_REV       0x024
#define HI_CHIP_MINOR_FEATURE_0 0x034
#define HI_MEMORY_COUNTERS 0x03c
#define PM_POWER_CONTROLS 0x104
#define PM_PULSE_EATER    0x10c

#define AQ_MEMORY_FE_PAGE_TABLE  0x400
#define AQ_MEMORY_TX_PAGE_TABLE  0x404
#define AQ_MEMORY_PE_PAGE_TABLE  0x408
#define AQ_MEMORY_PEZ_PAGE_TABLE 0x40c
#define AQ_MEMORY_RA_PAGE_TABLE  0x410
#define AQ_INTERRUPT_ENABLE       0x014
#define FE_COMMAND_ADDRESS        0x654
#define FE_COMMAND_CONTROL        0x658

#define HI_CLOCK_CONTROL_SOFT_RESET (1U << 12)
#define HI_CLOCK_CONTROL_IDLE_3D    (1U << 16)
#define HI_CLOCK_CONTROL_IDLE_2D    (1U << 17)
#define HI_CLOCK_CONTROL_ISOLATE    (1U << 19)
#define HI_IDLE_STATE_FE             (1U << 0)
#define FE_COMMAND_ENABLE            (1U << 16)

/*
 * Reference-board values, deliberately kept behind the profile property.
 * Gateworks' published i.MX6Q etnaviv log reports GC2000 rev 0x5108.
 * It is not an XDJ-XZ register capture.  No reference feature mask was
 * established, so the profile reports the unavailable masks as zero.
 */
#define GC2000_REFERENCE_MODEL       0x2000
#define GC2000_REFERENCE_REVISION    0x5108

static const hwaddr imx6_gc2000_diag_programming_order[] = {
    AQ_MEMORY_FE_PAGE_TABLE,
    AQ_MEMORY_RA_PAGE_TABLE,
    AQ_MEMORY_TX_PAGE_TABLE,
    AQ_MEMORY_PE_PAGE_TABLE,
    AQ_MEMORY_PEZ_PAGE_TABLE,
    AQ_INTERRUPT_ENABLE,
    FE_COMMAND_ADDRESS,
    FE_COMMAND_CONTROL,
};

static bool imx6_gc2000_diag_idle(const IMX6GC2000DiagState *s)
{
    return s->reset_complete &&
           !(s->clock_control & (HI_CLOCK_CONTROL_SOFT_RESET |
                                 HI_CLOCK_CONTROL_ISOLATE));
}

static bool imx6_gc2000_diag_check_width(hwaddr offset, unsigned size,
                                         const char *operation)
{
    if (size == sizeof(uint32_t)) {
        return true;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  TYPE_IMX6_GC2000_DIAG ": unsupported %s width %u at "
                  "offset 0x%" HWADDR_PRIx "\n", operation, size, offset);
    return false;
}

static void imx6_gc2000_diag_note_programming(IMX6GC2000DiagState *s,
                                               hwaddr offset, uint32_t value)
{
    if (s->programming_step >= ARRAY_SIZE(imx6_gc2000_diag_programming_order) ||
        offset != imx6_gc2000_diag_programming_order[s->programming_step]) {
        s->programming_order_valid = false;
        qemu_log_mask(LOG_GUEST_ERROR,
                      TYPE_IMX6_GC2000_DIAG
                      ": H4 programming order mismatch at offset 0x%"
                      HWADDR_PRIx " (step %u)\n", offset,
                      s->programming_step);
        return;
    }

    s->programming_step++;
    if (offset == FE_COMMAND_CONTROL) {
        /* Source encodes ENABLE in bit 16 and a nonzero prefetch count [15:0]. */
        s->fe_start_shape_valid = (value & FE_COMMAND_ENABLE) &&
                                  (value & UINT16_MAX);
    }
}

static uint64_t imx6_gc2000_diag_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    IMX6GC2000DiagState *s = opaque;

    if (!imx6_gc2000_diag_check_width(offset, size, "read")) {
        return 0;
    }

    switch (offset) {
    case HI_CLOCK_CONTROL:
        return s->clock_control |
               (imx6_gc2000_diag_idle(s) ?
                HI_CLOCK_CONTROL_IDLE_3D | HI_CLOCK_CONTROL_IDLE_2D : 0);
    case HI_IDLE_STATE:
        return imx6_gc2000_diag_idle(s) ? HI_IDLE_STATE_FE : 0;
    case HI_CHIP_IDENTITY:
        if (s->diagnostic_reference_identity) {
            /* Zero selects the normal (non-legacy) identify path only. */
            return 0;
        }
        break;
    case HI_CHIP_MODEL:
        if (s->diagnostic_reference_identity) {
            return GC2000_REFERENCE_MODEL;
        }
        break;
    case HI_CHIP_REV:
        if (s->diagnostic_reference_identity) {
            return GC2000_REFERENCE_REVISION;
        }
        break;
    case HI_CHIP_FEATURE:
    case HI_CHIP_MINOR_FEATURE_0:
        if (s->diagnostic_reference_identity) {
            /* Unknown XDJ-XZ feature masks are intentionally not invented. */
            return 0;
        }
        break;
    case AQ_MEMORY_FE_PAGE_TABLE:
        return s->mmu_fe_page_table;
    case AQ_MEMORY_RA_PAGE_TABLE:
        return s->mmu_ra_page_table;
    case AQ_MEMORY_TX_PAGE_TABLE:
        return s->mmu_tx_page_table;
    case AQ_MEMORY_PE_PAGE_TABLE:
        return s->mmu_pe_page_table;
    case AQ_MEMORY_PEZ_PAGE_TABLE:
        return s->mmu_pez_page_table;
    case AQ_INTERRUPT_ENABLE:
        return s->event_enable;
    case FE_COMMAND_ADDRESS:
        return s->fe_address;
    case FE_COMMAND_CONTROL:
        return s->fe_control;
    default:
        break;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  TYPE_IMX6_GC2000_DIAG
                  ": unsupported read at offset 0x%" HWADDR_PRIx "\n",
                  offset);
    /* Match the prior unmapped-window observation without inventing ID. */
    return 0;
}

static void imx6_gc2000_diag_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    IMX6GC2000DiagState *s = opaque;
    uint32_t old;

    if (!imx6_gc2000_diag_check_width(offset, size, "write")) {
        return;
    }

    switch (offset) {
    case PM_POWER_CONTROLS:
        s->power_controls = value;
        s->power_controls_disabled = value == 0;
        break;
    case PM_PULSE_EATER:
        old = s->pulse_eater;
        s->pulse_eater = value;
        if (!(old & 1) && (s->pulse_eater & 1)) {
            s->pulse_eater_asserted = true;
        }
        if (s->pulse_eater_asserted && (old & 1) &&
            !(s->pulse_eater & 1)) {
            s->pulse_eater_pulsed = true;
        }
        break;
    case HI_CLOCK_CONTROL:
        s->clock_control = value &
            ~(HI_CLOCK_CONTROL_IDLE_3D | HI_CLOCK_CONTROL_IDLE_2D);

        if (s->power_controls_disabled && s->pulse_eater_pulsed &&
            (s->clock_control & HI_CLOCK_CONTROL_ISOLATE)) {
            s->isolate_seen = true;
        }
        if (s->isolate_seen &&
            (s->clock_control & HI_CLOCK_CONTROL_SOFT_RESET)) {
            s->soft_reset_seen = true;
        }
        if (s->soft_reset_seen &&
            !(s->clock_control & HI_CLOCK_CONTROL_SOFT_RESET) &&
            !(s->clock_control & HI_CLOCK_CONTROL_ISOLATE)) {
            s->reset_complete = true;
        }
        break;
    case HI_MEMORY_COUNTERS:
        /*
         * InitializeHardware() resets the counters with ~0U, then zero
         * (Pioneer gc_hal_kernel_hardware.c:1121-1130).  This is strictly
         * an accepted local reset sequence: it neither exposes counters nor
         * causes a command, DMA, interrupt, or idle transition.
         */
        if ((uint32_t)value == UINT32_MAX) {
            s->memory_counter_reset_armed = true;
            s->memory_counters_reset_complete = false;
        } else if (s->memory_counter_reset_armed && (uint32_t)value == 0) {
            s->memory_counter_reset_armed = false;
            s->memory_counters_reset_complete = true;
        } else {
            s->memory_counter_reset_armed = false;
            s->memory_counters_reset_complete = false;
            qemu_log_mask(LOG_GUEST_ERROR,
                          TYPE_IMX6_GC2000_DIAG
                          ": unsupported counter-reset value 0x%08" PRIx64
                          " at offset 0x%03x\n", value, HI_MEMORY_COUNTERS);
        }
        break;
    case AQ_MEMORY_FE_PAGE_TABLE:
        s->mmu_fe_page_table = value;
        imx6_gc2000_diag_note_programming(s, offset, value);
        break;
    case AQ_MEMORY_RA_PAGE_TABLE:
        s->mmu_ra_page_table = value;
        imx6_gc2000_diag_note_programming(s, offset, value);
        break;
    case AQ_MEMORY_TX_PAGE_TABLE:
        s->mmu_tx_page_table = value;
        imx6_gc2000_diag_note_programming(s, offset, value);
        break;
    case AQ_MEMORY_PE_PAGE_TABLE:
        s->mmu_pe_page_table = value;
        imx6_gc2000_diag_note_programming(s, offset, value);
        break;
    case AQ_MEMORY_PEZ_PAGE_TABLE:
        s->mmu_pez_page_table = value;
        imx6_gc2000_diag_note_programming(s, offset, value);
        break;
    case AQ_INTERRUPT_ENABLE:
        s->event_enable = value;
        imx6_gc2000_diag_note_programming(s, offset, value);
        break;
    case FE_COMMAND_ADDRESS:
        s->fe_address = value;
        imx6_gc2000_diag_note_programming(s, offset, value);
        break;
    case FE_COMMAND_CONTROL:
        s->fe_control = value;
        imx6_gc2000_diag_note_programming(s, offset, value);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      TYPE_IMX6_GC2000_DIAG
                      ": unsupported write 0x%08" PRIx64
                      " at offset 0x%" HWADDR_PRIx "\n",
                      value, offset);
        break;
    }
}

static const MemoryRegionOps imx6_gc2000_diag_ops = {
    .read = imx6_gc2000_diag_read,
    .write = imx6_gc2000_diag_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = sizeof(uint8_t),
        .max_access_size = sizeof(uint32_t),
    },
};

static void imx6_gc2000_diag_reset(DeviceState *dev)
{
    IMX6GC2000DiagState *s = IMX6_GC2000_DIAG(dev);

    s->clock_control = 0;
    s->power_controls = 0;
    s->pulse_eater = 0;
    s->mmu_fe_page_table = 0;
    s->mmu_ra_page_table = 0;
    s->mmu_tx_page_table = 0;
    s->mmu_pe_page_table = 0;
    s->mmu_pez_page_table = 0;
    s->event_enable = 0;
    s->fe_address = 0;
    s->fe_control = 0;
    s->programming_step = 0;
    s->memory_counter_reset_armed = false;
    s->memory_counters_reset_complete = false;
    s->power_controls_disabled = false;
    s->pulse_eater_asserted = false;
    s->pulse_eater_pulsed = false;
    s->isolate_seen = false;
    s->soft_reset_seen = false;
    s->reset_complete = false;
    s->programming_order_valid = true;
    s->fe_start_shape_valid = false;
}

static void imx6_gc2000_diag_init(Object *obj)
{
    IMX6GC2000DiagState *s = IMX6_GC2000_DIAG(obj);

    memory_region_init_io(&s->iomem, obj, &imx6_gc2000_diag_ops, s,
                          TYPE_IMX6_GC2000_DIAG, IMX6_GC2000_DIAG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
}

static const VMStateDescription vmstate_imx6_gc2000_diag = {
    .name = TYPE_IMX6_GC2000_DIAG,
    .version_id = 3,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(clock_control, IMX6GC2000DiagState),
        VMSTATE_UINT32(power_controls, IMX6GC2000DiagState),
        VMSTATE_UINT32(pulse_eater, IMX6GC2000DiagState),
        VMSTATE_BOOL(power_controls_disabled, IMX6GC2000DiagState),
        VMSTATE_BOOL(pulse_eater_asserted, IMX6GC2000DiagState),
        VMSTATE_BOOL(pulse_eater_pulsed, IMX6GC2000DiagState),
        VMSTATE_BOOL(isolate_seen, IMX6GC2000DiagState),
        VMSTATE_BOOL(soft_reset_seen, IMX6GC2000DiagState),
        VMSTATE_BOOL(reset_complete, IMX6GC2000DiagState),
        VMSTATE_UINT32_V(mmu_fe_page_table, IMX6GC2000DiagState, 2),
        VMSTATE_UINT32_V(mmu_ra_page_table, IMX6GC2000DiagState, 2),
        VMSTATE_UINT32_V(mmu_tx_page_table, IMX6GC2000DiagState, 2),
        VMSTATE_UINT32_V(mmu_pe_page_table, IMX6GC2000DiagState, 2),
        VMSTATE_UINT32_V(mmu_pez_page_table, IMX6GC2000DiagState, 2),
        VMSTATE_UINT32_V(event_enable, IMX6GC2000DiagState, 2),
        VMSTATE_UINT32_V(fe_address, IMX6GC2000DiagState, 2),
        VMSTATE_UINT32_V(fe_control, IMX6GC2000DiagState, 2),
        VMSTATE_UINT8_V(programming_step, IMX6GC2000DiagState, 2),
        VMSTATE_BOOL_V(programming_order_valid, IMX6GC2000DiagState, 2),
        VMSTATE_BOOL_V(fe_start_shape_valid, IMX6GC2000DiagState, 2),
        VMSTATE_BOOL_V(memory_counter_reset_armed, IMX6GC2000DiagState, 3),
        VMSTATE_BOOL_V(memory_counters_reset_complete,
                       IMX6GC2000DiagState, 3),
        VMSTATE_END_OF_LIST()
    },
};

static void imx6_gc2000_diag_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, imx6_gc2000_diag_reset);
    dc->vmsd = &vmstate_imx6_gc2000_diag;
    dc->desc = "i.MX6 3D Vivante reset/idle diagnostic model";
}

static const TypeInfo imx6_gc2000_diag_type_info = {
    .name = TYPE_IMX6_GC2000_DIAG,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMX6GC2000DiagState),
    .instance_init = imx6_gc2000_diag_init,
    .class_init = imx6_gc2000_diag_class_init,
};

static void imx6_gc2000_diag_register_types(void)
{
    type_register_static(&imx6_gc2000_diag_type_info);
}

type_init(imx6_gc2000_diag_register_types)
