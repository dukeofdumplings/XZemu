/* Minimal i.MX6 BCH register model for the XDJ-XZ Phase-A platform. */
#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/misc/imx6_bch.h"
#include "hw/misc/imx6_gpmi_reset.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define BCH_CTRL       0x000
#define BCH_CTRL_SET   0x004
#define BCH_CTRL_CLR   0x008
#define BCH_CTRL_TOG   0x00c
#define BCH_CLKGATE    (1U << 30)
#define BCH_SFTRST     (1U << 31)
#define BCH_COMPLETE_IRQ (1U << 0)
#define BCH_COMPLETE_IRQ_EN (1U << 8)
#define BCH_STATUS0    0x010
#define BCH_LAYOUTSELECT 0x070
#define BCH_FLASH0LAYOUT0 0x080
#define BCH_FLASH0LAYOUT1 0x090
#define BCH_CTRL_WRITABLE (BCH_SFTRST | BCH_CLKGATE | BCH_COMPLETE_IRQ_EN)

static void imx6_bch_update_irq(IMX6BCHState *s)
{
    qemu_set_irq(s->irq, s->complete_irq &&
                 !!(s->ctrl_config & BCH_COMPLETE_IRQ_EN));
}

static void imx6_bch_soft_reset(IMX6BCHState *s)
{
    s->status0 = 0;
    s->complete_irq = false;
    s->status0_read_logged = false;
}

void imx6_bch_complete(IMX6BCHState *s, uint32_t status0)
{
    imx6_gpmi_diagnostic_bch_completion(s->gpmi);
    s->status0 = status0;
    s->complete_irq = true;
    imx6_bch_update_irq(s);
    qemu_log_mask(LOG_GUEST_ERROR,
                  "XZ_BCH: complete status0=0x%08x complete_irq_en=%u\n",
                  status0, !!(s->ctrl_config & BCH_COMPLETE_IRQ_EN));
}

bool imx6_bch_is_operational(const IMX6BCHState *s)
{
    return !(s->ctrl_config & (BCH_SFTRST | BCH_CLKGATE)) &&
           s->regs[BCH_LAYOUTSELECT / sizeof(uint32_t)] == 0 &&
           s->regs[BCH_FLASH0LAYOUT0 / sizeof(uint32_t)] == 0x030a2080 &&
           s->regs[BCH_FLASH0LAYOUT1 / sizeof(uint32_t)] == 0x08402080;
}

static uint64_t imx6_bch_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX6BCHState *s = opaque;

    imx6_gpmi_diagnostic_access(s->gpmi, IMX6_NAND_DIAG_BCH,
                                 offset, size, false);

    if (offset == BCH_CTRL) {
        return s->ctrl_config | (s->complete_irq ? BCH_COMPLETE_IRQ : 0);
    }
    if (offset == BCH_STATUS0) {
        if (!s->status0_read_logged) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "XZ_BCH: status0-read value=0x%08x\n", s->status0);
            s->status0_read_logged = true;
        }
        return s->status0;
    }
    return s->regs[offset / sizeof(uint32_t)];
}

static void imx6_bch_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    IMX6BCHState *s = opaque;
    uint32_t bits = value;

    imx6_gpmi_diagnostic_access(s->gpmi, IMX6_NAND_DIAG_BCH,
                                 offset, size, true);

    switch (offset) {
    case BCH_CTRL:
        s->ctrl_config = bits & BCH_CTRL_WRITABLE;
        if (bits & BCH_SFTRST) {
            s->ctrl_config |= BCH_CLKGATE;
            imx6_bch_soft_reset(s);
        }
        break;
    case BCH_CTRL_SET:
        s->ctrl_config |= bits & BCH_CTRL_WRITABLE;
        if (bits & BCH_SFTRST) {
            s->ctrl_config |= BCH_CLKGATE;
            imx6_bch_soft_reset(s);
        }
        break;
    case BCH_CTRL_CLR:
        s->ctrl_config &= ~(bits & BCH_CTRL_WRITABLE);
        if (bits & BCH_COMPLETE_IRQ) {
            s->complete_irq = false;
        }
        break;
    case BCH_CTRL_TOG:
        if (!(s->ctrl_config & BCH_SFTRST) && (bits & BCH_SFTRST)) {
            imx6_bch_soft_reset(s);
        }
        s->ctrl_config ^= bits & BCH_CTRL_WRITABLE;
        if (s->ctrl_config & BCH_SFTRST) {
            s->ctrl_config |= BCH_CLKGATE;
        }
        break;
    case BCH_STATUS0:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_BCH: ignored guest STATUS0 write value=0x%08x\n", bits);
        return;
    default:
        s->regs[offset / sizeof(uint32_t)] = bits;
        break;
    }
    imx6_bch_update_irq(s);
    qemu_log_mask(LOG_GUEST_ERROR,
                  "XZ_BCH: write offset=0x%03" HWADDR_PRIx
                  " value=0x%08x ctrl=0x%08x complete=%u\n",
                  offset, bits, s->ctrl_config, s->complete_irq);
}

static const MemoryRegionOps imx6_bch_ops = {
    .read = imx6_bch_read,
    .write = imx6_bch_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void imx6_bch_reset(DeviceState *dev)
{
    IMX6BCHState *s = IMX6_BCH(dev);
    memset(s->regs, 0, sizeof(s->regs));
    s->ctrl_config = 0;
    s->status0 = 0;
    s->complete_irq = false;
    s->status0_read_logged = false;
    imx6_bch_update_irq(s);
}

void imx6_bch_set_gpmi(IMX6BCHState *s, IMX6GPMIResetState *gpmi)
{
    s->gpmi = gpmi;
}

static void imx6_bch_init(Object *obj)
{
    IMX6BCHState *s = IMX6_BCH(obj);
    memory_region_init_io(&s->iomem, obj, &imx6_bch_ops, s,
                          TYPE_IMX6_BCH, IMX6_BCH_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
}

static int imx6_bch_post_load(void *opaque, int version_id)
{
    IMX6BCHState *s = opaque;

    imx6_bch_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_imx6_bch = {
    .name = TYPE_IMX6_BCH,
    .version_id = 2,
    .minimum_version_id = 1,
    .post_load = imx6_bch_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX6BCHState,
                             IMX6_BCH_SIZE / sizeof(uint32_t)),
        VMSTATE_UINT32_V(ctrl_config, IMX6BCHState, 2),
        VMSTATE_UINT32_V(status0, IMX6BCHState, 2),
        VMSTATE_BOOL_V(complete_irq, IMX6BCHState, 2),
        VMSTATE_BOOL_V(status0_read_logged, IMX6BCHState, 2),
        VMSTATE_END_OF_LIST(),
    },
};

static void imx6_bch_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    device_class_set_legacy_reset(dc, imx6_bch_reset);
    dc->vmsd = &vmstate_imx6_bch;
    dc->desc = "i.MX6 BCH register block";
}

static const TypeInfo imx6_bch_type_info = {
    .name = TYPE_IMX6_BCH,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMX6BCHState),
    .instance_init = imx6_bch_init,
    .class_init = imx6_bch_class_init,
};

static void imx6_bch_register_types(void)
{
    type_register_static(&imx6_bch_type_info);
}

type_init(imx6_bch_register_types)
