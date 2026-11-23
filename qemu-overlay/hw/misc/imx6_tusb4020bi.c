/*
 * Minimal XDJ-XZ TUSB4020BI SMBus configuration target.
 *
 * It implements precisely the byte-data writes made by Pioneer’s active
 * setup_usbhub_4ch.sh path. This is a firmware-compatibility surface, not a
 * claim that the physical hub accepts arbitrary SMBus byte-data operations.
 */
#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "hw/misc/imx6_tusb4020bi.h"
#include "qemu/module.h"

typedef struct IMX6XDJTUSB4020BIState {
    I2CSlave parent_obj;

    uint8_t regs[0x100];
    uint8_t reg;
    uint8_t pos;
    uint32_t accepted_writes;
} IMX6XDJTUSB4020BIState;

#define IMX6_XDJ_TUSB4020BI(obj) \
    OBJECT_CHECK(IMX6XDJTUSB4020BIState, (obj), TYPE_IMX6_XDJ_TUSB4020BI)

static bool tusb4020bi_active_write(uint8_t reg, uint8_t value)
{
    switch (reg) {
    case 0x22:
    case 0x23:
    case 0x24:
    case 0x30:
    case 0x50:
    case 0x90:
        return value == 0x00;
    case 0xf8:
        return value == 0x01;
    default:
        return false;
    }
}

static int tusb4020bi_event(I2CSlave *slave, enum i2c_event event)
{
    IMX6XDJTUSB4020BIState *s = IMX6_XDJ_TUSB4020BI(slave);

    if (event == I2C_START_SEND) {
        s->pos = 0;
    }
    return 0;
}

static int tusb4020bi_send(I2CSlave *slave, uint8_t value)
{
    IMX6XDJTUSB4020BIState *s = IMX6_XDJ_TUSB4020BI(slave);

    if (s->pos++ == 0) {
        s->reg = value;
        return 0;
    }
    if (!tusb4020bi_active_write(s->reg, value)) {
        return -1;
    }
    if (s->reg == 0xf8) {
        /* cfgActive is write-one-to-clear; SMBus reset is not modelled. */
        s->regs[s->reg] &= ~0x01;
    } else {
        s->regs[s->reg] = value;
    }
    s->accepted_writes++;
    return 0;
}

static uint8_t tusb4020bi_recv(I2CSlave *slave)
{
    IMX6XDJTUSB4020BIState *s = IMX6_XDJ_TUSB4020BI(slave);

    return s->regs[s->reg++];
}

static void tusb4020bi_reset(DeviceState *dev)
{
    IMX6XDJTUSB4020BIState *s = IMX6_XDJ_TUSB4020BI(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[0x01] = 0x51; /* VID 0x0451 */
    s->regs[0x02] = 0x04;
    s->regs[0x03] = 0x25; /* Descriptor PID-low = 0x25 XOR 0x02. */
    s->regs[0x04] = 0x80; /* Descriptor PID 0x8027. */
    s->regs[0x07] = 0x03; /* Both downstream sockets removable. */
    s->regs[0x08] = 0x03; /* Both downstream ports used. */
    s->regs[0xf8] = 0x01; /* cfgActive until Pioneer’s final setup write. */
    s->reg = 0;
    s->pos = 0;
    s->accepted_writes = 0;
}

static void tusb4020bi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(klass);

    device_class_set_legacy_reset(dc, tusb4020bi_reset);
    sc->event = tusb4020bi_event;
    sc->send = tusb4020bi_send;
    sc->recv = tusb4020bi_recv;
}

static const TypeInfo tusb4020bi_info = {
    .name = TYPE_IMX6_XDJ_TUSB4020BI,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(IMX6XDJTUSB4020BIState),
    .class_init = tusb4020bi_class_init,
};

static void tusb4020bi_register_types(void)
{
    type_register_static(&tusb4020bi_info);
}

type_init(tusb4020bi_register_types)
