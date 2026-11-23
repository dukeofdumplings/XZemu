/*
 * Opt-in diagnostic state machine for the i.MX6 GC320 2D reset path.
 *
 * Source contract: Pioneer XDJ-XZ Linux 3.0.101 Galcore 4.6.9.9754,
 * gc_hal_kernel_hardware.c:gckHARDWARE_Construct() and _ResetGPU().
 * The model profile is deliberately provisional: it supplies only gcv320
 * (0x0320), then permits the driver's 0x414 AXI low-byte RMW and reset poll.
 */
#include "qemu/osdep.h"
#include "hw/misc/imx6_gc320_diag.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define HI_CLOCK_CONTROL 0x000
#define HI_IDLE_STATE     0x004
#define CHIP_MODEL        0x020
#define PM_POWER_CONTROLS 0x104
#define PM_PULSE_EATER    0x10c
#define AQ_AXI_OUTSTANDING 0x414

#define HI_CLOCK_CONTROL_SOFT_RESET (1U << 12)
#define HI_CLOCK_CONTROL_IDLE_3D    (1U << 16)
#define HI_CLOCK_CONTROL_IDLE_2D    (1U << 17)
#define HI_CLOCK_CONTROL_ISOLATE    (1U << 19)
#define HI_IDLE_STATE_FE             (1U << 0)
#define PM_DISABLE_CLOCK_GATING       (1U << 15)
#define GC320_MODEL                  0x0320
#define AXI_LOW_BYTE                 0x10

static bool imx6_gc320_diag_idle(const IMX6GC320DiagState *s)
{
    return s->reset_complete && s->reset_sequence_valid;
}

static bool imx6_gc320_diag_check_width(hwaddr offset, unsigned size,
                                        const char *operation)
{
    if (size == sizeof(uint32_t)) {
        return true;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  TYPE_IMX6_GC320_DIAG ": unsupported %s width %u at "
                  "offset 0x%" HWADDR_PRIx "\n", operation, size, offset);
    return false;
}

static void imx6_gc320_diag_bad_sequence(IMX6GC320DiagState *s,
                                          hwaddr offset, uint32_t value)
{
    s->reset_sequence_valid = false;
    qemu_log_mask(LOG_GUEST_ERROR,
                  TYPE_IMX6_GC320_DIAG ": reset ordering mismatch at step %u "
                  "(offset 0x%" HWADDR_PRIx ", value 0x%08x)\n",
                  s->reset_step, offset, value);
}

static void imx6_gc320_diag_begin_reset(IMX6GC320DiagState *s, uint32_t value)
{
    s->power_controls = value;
    s->pulse_eater = 0;
    s->reset_step = 1;
    s->reset_in_progress = true;
    s->reset_sequence_valid = value == 0;
    s->reset_complete = false;
    if (value != 0) {
        imx6_gc320_diag_bad_sequence(s, PM_POWER_CONTROLS, value);
    }
}

static void imx6_gc320_diag_reset_pulse(IMX6GC320DiagState *s, uint32_t value)
{
    static const uint32_t expected[] = {
        0x015b0880, 0x015b0881, 0x015b0880,
    };

    s->pulse_eater = value;
    if (!s->reset_in_progress || s->reset_step < 1 || s->reset_step > 3 ||
        value != expected[s->reset_step - 1]) {
        imx6_gc320_diag_bad_sequence(s, PM_PULSE_EATER, value);
        return;
    }
    s->reset_step++;
}

static void imx6_gc320_diag_reset_clock(IMX6GC320DiagState *s, uint32_t value)
{
    static const uint32_t expected[] = {
        0, 0, 0, 0,
        0x00000b00, 0x00000900, 0x00080900, 0x00081900,
        0x00080900, 0x00000900,
    };

    s->clock_control = value &
        ~(HI_CLOCK_CONTROL_IDLE_3D | HI_CLOCK_CONTROL_IDLE_2D);
    if (!s->reset_in_progress) {
        /* gckHARDWARE_Construct() writes 0x900 before identification. */
        return;
    }
    if (s->reset_step < 4 || s->reset_step > 9 ||
        value != expected[s->reset_step]) {
        imx6_gc320_diag_bad_sequence(s, HI_CLOCK_CONTROL, value);
        return;
    }
    s->reset_step++;
    if (s->reset_step == 10 && s->reset_sequence_valid) {
        s->reset_complete = true;
        s->reset_in_progress = false;
    }
}

static uint64_t imx6_gc320_diag_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    IMX6GC320DiagState *s = opaque;

    if (!imx6_gc320_diag_check_width(offset, size, "read")) {
        return 0;
    }
    switch (offset) {
    case HI_CLOCK_CONTROL:
        return s->clock_control | (imx6_gc320_diag_idle(s) ?
               HI_CLOCK_CONTROL_IDLE_3D | HI_CLOCK_CONTROL_IDLE_2D : 0);
    case HI_IDLE_STATE:
        return imx6_gc320_diag_idle(s) ? HI_IDLE_STATE_FE : 0;
    case CHIP_MODEL:
        if (s->diagnostic_gc320_model) {
            return GC320_MODEL;
        }
        break;
    case PM_POWER_CONTROLS:
        return s->power_controls;
    case PM_PULSE_EATER:
        return s->pulse_eater;
    case AQ_AXI_OUTSTANDING:
        if (s->diagnostic_gc320_model) {
            s->axi_read_observed = true;
            return s->axi_outstanding;
        }
        break;
    default:
        break;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  TYPE_IMX6_GC320_DIAG ": unsupported read at offset 0x%"
                  HWADDR_PRIx "\n", offset);
    return 0;
}

static void imx6_gc320_diag_write_axi(IMX6GC320DiagState *s, uint32_t value)
{
    if (!s->diagnostic_gc320_model || !s->axi_read_observed ||
        (value & ~UINT32_C(0xff)) != (s->axi_outstanding & ~UINT32_C(0xff)) ||
        (value & UINT32_C(0xff)) != AXI_LOW_BYTE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      TYPE_IMX6_GC320_DIAG ": unsupported AXI outstanding "
                      "write 0x%08x\n", value);
        return;
    }
    s->axi_outstanding = (s->axi_outstanding & ~UINT32_C(0xff)) |
                         (value & UINT32_C(0xff));
    s->axi_low_byte_programmed = true;
}

static void imx6_gc320_diag_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    IMX6GC320DiagState *s = opaque;
    uint32_t word = value;

    if (!imx6_gc320_diag_check_width(offset, size, "write")) {
        return;
    }
    switch (offset) {
    case HI_CLOCK_CONTROL:
        imx6_gc320_diag_reset_clock(s, word);
        break;
    case PM_POWER_CONTROLS:
        /*
         * Pioneer first writes zero here to begin _ResetGPU(), then, after
         * successful reset, reads this register and sets bit 15.  Preserve
         * the completed reset state for that observed module-control RMW.
         */
        if (s->reset_complete && !s->reset_in_progress &&
            word == PM_DISABLE_CLOCK_GATING) {
            s->power_controls = word;
        } else {
            imx6_gc320_diag_begin_reset(s, word);
        }
        break;
    case PM_PULSE_EATER:
        imx6_gc320_diag_reset_pulse(s, word);
        break;
    case AQ_AXI_OUTSTANDING:
        imx6_gc320_diag_write_axi(s, word);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      TYPE_IMX6_GC320_DIAG ": unsupported write 0x%08x at "
                      "offset 0x%" HWADDR_PRIx "\n", word, offset);
        break;
    }
}

static const MemoryRegionOps imx6_gc320_diag_ops = {
    .read = imx6_gc320_diag_read,
    .write = imx6_gc320_diag_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = sizeof(uint8_t),
        .max_access_size = sizeof(uint32_t),
    },
};

static void imx6_gc320_diag_reset(DeviceState *dev)
{
    IMX6GC320DiagState *s = IMX6_GC320_DIAG(dev);

    s->clock_control = 0;
    s->power_controls = 0;
    s->pulse_eater = 0;
    s->axi_outstanding = 0;
    s->reset_step = 0;
    s->reset_in_progress = false;
    s->reset_sequence_valid = true;
    s->reset_complete = false;
    s->axi_read_observed = false;
    s->axi_low_byte_programmed = false;
}

static void imx6_gc320_diag_init(Object *obj)
{
    IMX6GC320DiagState *s = IMX6_GC320_DIAG(obj);

    memory_region_init_io(&s->iomem, obj, &imx6_gc320_diag_ops, s,
                          TYPE_IMX6_GC320_DIAG, IMX6_GC320_DIAG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
}

static const VMStateDescription vmstate_imx6_gc320_diag = {
    .name = TYPE_IMX6_GC320_DIAG,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(clock_control, IMX6GC320DiagState),
        VMSTATE_UINT32(power_controls, IMX6GC320DiagState),
        VMSTATE_UINT32(pulse_eater, IMX6GC320DiagState),
        VMSTATE_UINT32(axi_outstanding, IMX6GC320DiagState),
        VMSTATE_UINT8(reset_step, IMX6GC320DiagState),
        VMSTATE_BOOL(reset_in_progress, IMX6GC320DiagState),
        VMSTATE_BOOL(reset_sequence_valid, IMX6GC320DiagState),
        VMSTATE_BOOL(reset_complete, IMX6GC320DiagState),
        VMSTATE_BOOL(axi_read_observed, IMX6GC320DiagState),
        VMSTATE_BOOL(axi_low_byte_programmed, IMX6GC320DiagState),
        VMSTATE_END_OF_LIST()
    },
};

static void imx6_gc320_diag_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, imx6_gc320_diag_reset);
    dc->vmsd = &vmstate_imx6_gc320_diag;
    dc->desc = "i.MX6 GC320 reset/idle diagnostic model";
}

static const TypeInfo imx6_gc320_diag_type_info = {
    .name = TYPE_IMX6_GC320_DIAG,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMX6GC320DiagState),
    .instance_init = imx6_gc320_diag_init,
    .class_init = imx6_gc320_diag_class_init,
};

static void imx6_gc320_diag_register_types(void)
{
    type_register_static(&imx6_gc320_diag_type_info);
}

type_init(imx6_gc320_diag_register_types)
