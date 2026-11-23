/* i.MX6 AUDMUX v2 routing register model for the XDJ-XZ. */
#include "qemu/osdep.h"
#include "hw/misc/imx6_audmux.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "system/memory.h"

static bool imx6_audmux_decode(hwaddr offset, unsigned int *port,
                               bool *pdcr)
{
    if (offset >= IMX6_AUDMUX_PORTS * 8 || (offset & 3)) {
        return false;
    }
    *port = offset / 8;
    *pdcr = !!(offset & 4);
    return true;
}

static uint64_t imx6_audmux_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX6AUDMUXState *s = opaque;
    unsigned int port;
    bool pdcr;

    if (!imx6_audmux_decode(offset, &port, &pdcr)) {
        return 0;
    }
    return pdcr ? s->pdcr[port] : s->ptcr[port];
}

static void imx6_audmux_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    IMX6AUDMUXState *s = opaque;
    unsigned int port;
    bool pdcr;

    if (!imx6_audmux_decode(offset, &port, &pdcr)) {
        return;
    }
    if (pdcr) {
        s->pdcr[port] = value;
    } else {
        s->ptcr[port] = value;
    }
}

static const MemoryRegionOps imx6_audmux_ops = {
    .read = imx6_audmux_read,
    .write = imx6_audmux_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void imx6_audmux_reset(DeviceState *dev)
{
    IMX6AUDMUXState *s = IMX6_AUDMUX(dev);

    memset(s->ptcr, 0, sizeof(s->ptcr));
    memset(s->pdcr, 0, sizeof(s->pdcr));
}

static void imx6_audmux_init(Object *obj)
{
    IMX6AUDMUXState *s = IMX6_AUDMUX(obj);

    memory_region_init_io(&s->iomem, obj, &imx6_audmux_ops, s,
                          TYPE_IMX6_AUDMUX, IMX6_AUDMUX_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
}

static const VMStateDescription vmstate_imx6_audmux = {
    .name = TYPE_IMX6_AUDMUX,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(ptcr, IMX6AUDMUXState, IMX6_AUDMUX_PORTS),
        VMSTATE_UINT32_ARRAY(pdcr, IMX6AUDMUXState, IMX6_AUDMUX_PORTS),
        VMSTATE_END_OF_LIST(),
    },
};

static void imx6_audmux_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, imx6_audmux_reset);
    dc->vmsd = &vmstate_imx6_audmux;
    dc->desc = "i.MX6 AUDMUX v2 routing registers";
}

static const TypeInfo imx6_audmux_type_info = {
    .name = TYPE_IMX6_AUDMUX,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMX6AUDMUXState),
    .instance_init = imx6_audmux_init,
    .class_init = imx6_audmux_class_init,
};

static void imx6_audmux_register_types(void)
{
    type_register_static(&imx6_audmux_type_info);
}

type_init(imx6_audmux_register_types)
