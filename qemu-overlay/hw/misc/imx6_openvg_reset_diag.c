/*
 * Opt-in OpenVG reset/idle diagnostic for the Pioneer XDJ-XZ Linux driver.
 *
 * Evidence boundary: gc_hal_kernel_hardware_vg.c:_ResetGPU() reads control
 * (+0x000), writes module control (+0x104), pulses control values 0x80000,
 * 0x81000, 0x80000, 0, then waits for idle bit 0 at +0x004 and both idle
 * bits 16/17 at +0x000.  No physical XDJ-XZ identity value is known, so all
 * identity and execution registers remain unsupported and read as zero.
 */
#include "qemu/osdep.h"
#include "hw/misc/imx6_openvg_reset_diag.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define HI_CLOCK_CONTROL          0x000
#define HI_IDLE_STATE              0x004
#define PM_POWER_CONTROLS          0x104
#define HI_CLOCK_SOFT_RESET        (1U << 12)
#define HI_CLOCK_IDLE_3D           (1U << 16)
#define HI_CLOCK_IDLE_2D           (1U << 17)
#define HI_CLOCK_ISOLATE           (1U << 19)
#define HI_IDLE_FE                 (1U << 0)

static bool imx6_openvg_check_width(hwaddr offset, unsigned size,
                                    const char *operation)
{
    if (size == sizeof(uint32_t)) {
        return true;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  TYPE_IMX6_OPENVG_RESET_DIAG ": unsupported %s width %u at "
                  "offset 0x%" HWADDR_PRIx "\n", operation, size, offset);
    return false;
}

static void imx6_openvg_bad_reset(IMX6OpenVGResetDiagState *s,
                                  hwaddr offset, uint32_t value)
{
    s->reset_sequence_valid = false;
    qemu_log_mask(LOG_GUEST_ERROR,
                  TYPE_IMX6_OPENVG_RESET_DIAG ": reset ordering mismatch at "
                  "step %u (offset 0x%" HWADDR_PRIx ", value 0x%08x)\n",
                  s->reset_step, offset, value);
}

static uint64_t imx6_openvg_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX6OpenVGResetDiagState *s = opaque;

    if (!imx6_openvg_check_width(offset, size, "read")) {
        return 0;
    }
    switch (offset) {
    case HI_CLOCK_CONTROL:
        return s->clock_control | (s->reset_complete ?
               HI_CLOCK_IDLE_3D | HI_CLOCK_IDLE_2D : 0);
    case HI_IDLE_STATE:
        return s->reset_complete ? HI_IDLE_FE : 0;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      TYPE_IMX6_OPENVG_RESET_DIAG ": unsupported read at "
                      "offset 0x%" HWADDR_PRIx "\n", offset);
        return 0;
    }
}

static void imx6_openvg_write_control(IMX6OpenVGResetDiagState *s,
                                      uint32_t value)
{
    static const uint32_t expected[] = {
        HI_CLOCK_ISOLATE,
        HI_CLOCK_ISOLATE | HI_CLOCK_SOFT_RESET,
        HI_CLOCK_ISOLATE,
        0,
    };

    if (s->reset_step == 0 || s->reset_step > ARRAY_SIZE(expected) ||
        value != expected[s->reset_step - 1]) {
        imx6_openvg_bad_reset(s, HI_CLOCK_CONTROL, value);
        return;
    }
    s->clock_control = value;
    s->reset_step++;
    if (s->reset_step == ARRAY_SIZE(expected) + 1 &&
        s->reset_sequence_valid) {
        s->reset_complete = true;
    }
}

static void imx6_openvg_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    IMX6OpenVGResetDiagState *s = opaque;
    uint32_t word = value;

    if (!imx6_openvg_check_width(offset, size, "write")) {
        return;
    }
    switch (offset) {
    case PM_POWER_CONTROLS:
        if (word != 0) {
            imx6_openvg_bad_reset(s, offset, word);
            return;
        }
        s->reset_step = 1;
        s->reset_sequence_valid = true;
        s->reset_complete = false;
        break;
    case HI_CLOCK_CONTROL:
        imx6_openvg_write_control(s, word);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      TYPE_IMX6_OPENVG_RESET_DIAG ": unsupported write "
                      "0x%08x at offset 0x%" HWADDR_PRIx "\n", word, offset);
        break;
    }
}

static const MemoryRegionOps imx6_openvg_ops = {
    .read = imx6_openvg_read,
    .write = imx6_openvg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = sizeof(uint8_t),
        .max_access_size = sizeof(uint32_t),
    },
};

static void imx6_openvg_reset(DeviceState *dev)
{
    IMX6OpenVGResetDiagState *s = IMX6_OPENVG_RESET_DIAG(dev);

    s->clock_control = 0;
    s->reset_step = 0;
    s->reset_sequence_valid = true;
    s->reset_complete = false;
}

static void imx6_openvg_init(Object *obj)
{
    IMX6OpenVGResetDiagState *s = IMX6_OPENVG_RESET_DIAG(obj);

    memory_region_init_io(&s->iomem, obj, &imx6_openvg_ops, s,
                          TYPE_IMX6_OPENVG_RESET_DIAG,
                          IMX6_OPENVG_RESET_DIAG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
}

static const VMStateDescription vmstate_imx6_openvg_reset_diag = {
    .name = TYPE_IMX6_OPENVG_RESET_DIAG,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(clock_control, IMX6OpenVGResetDiagState),
        VMSTATE_UINT8(reset_step, IMX6OpenVGResetDiagState),
        VMSTATE_BOOL(reset_sequence_valid, IMX6OpenVGResetDiagState),
        VMSTATE_BOOL(reset_complete, IMX6OpenVGResetDiagState),
        VMSTATE_END_OF_LIST()
    },
};

static void imx6_openvg_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, imx6_openvg_reset);
    dc->vmsd = &vmstate_imx6_openvg_reset_diag;
    dc->desc = "i.MX6 OpenVG reset/idle diagnostic model";
}

static const TypeInfo imx6_openvg_type_info = {
    .name = TYPE_IMX6_OPENVG_RESET_DIAG,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMX6OpenVGResetDiagState),
    .instance_init = imx6_openvg_init,
    .class_init = imx6_openvg_class_init,
};

static void imx6_openvg_register_types(void)
{
    type_register_static(&imx6_openvg_type_info);
}

type_init(imx6_openvg_register_types)
