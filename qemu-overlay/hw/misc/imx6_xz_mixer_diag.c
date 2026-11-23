/*
 * XDJ-XZ ECSPI5 mixer-ready diagnostic responder.
 *
 * Pioneer Linux registers /dev/subucom_spi_rdy4.0 on ECSPI5 CS0 and gates
 * each 56-byte transfer on GPIO1_12, whose board callback is active low.
 * RBP's SubMiconRx checks an X-25 CRC over the 52-byte payload after
 * reversing the byte order within each 32-bit word, then decodes MENU from
 * payload byte 19 bit 3.
 *
 * There is no captured physical idle mixer-ready frame yet.  (PNL is the
 * separate 20-byte ECSPI4-CS1 protocol.)  Therefore this device is explicit
 * synthetic diagnostic transport only: its other payload bytes are zero and
 * it does not claim to emulate the real mixer MCU.  The named controls below
 * are limited to exact-RBP binary recovered fields and are exposed through
 * QMP's qom-set command only when this explicit synthetic responder is on.
 */
#include "qemu/osdep.h"
#include "hw/misc/imx6_xz_mixer_diag.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/visitor.h"
#include "qemu/module.h"

/* Reflected CRC-16/X-25: common_crc16::crc in the supplied RBP analysis. */
static uint16_t xz_mixer_crc16_x25(const uint8_t *bytes, size_t length)
{
    uint16_t crc = 0xffff;
    size_t index;

    for (index = 0; index < length; index++) {
        unsigned bit;

        crc ^= bytes[index];
        for (bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : crc >> 1;
        }
    }
    return crc ^ 0xffff;
}

static void imx6_xz_mixer_build_frame(IMX6XZMixerDiagState *s,
                                      uint8_t frame[IMX6_XZ_MIXER_FRAME_BYTES])
{
    uint8_t crc_input[IMX6_XZ_MIXER_PAYLOAD_BYTES];
    uint16_t crc;
    size_t index;

    memset(frame, 0, IMX6_XZ_MIXER_FRAME_BYTES);
    /*
     * This deliberately unknown neutral payload is only sufficient for the
     * MENU transport experiment.  RBP may render unrelated defaults from
     * its zero fields (observed: WIDE tempo and 100.0% playback rate).  Those
     * UI values are not an intentionally modeled tempo/control contract.
     */
    frame[16] = s->rotary_buttons;
    frame[18] = s->navigation_buttons;
    frame[19] = s->head_buttons;
    frame[22] = s->rotary_count & 0xff;
    frame[23] = s->rotary_count >> 8;
    if (s->menu_high) {
        frame[19] |= 0x08;
    }

    /* RBP reverses each source word before handing it to common_crc16. */
    for (index = 0; index < IMX6_XZ_MIXER_PAYLOAD_BYTES; index += 4) {
        crc_input[index] = frame[index + 3];
        crc_input[index + 1] = frame[index + 2];
        crc_input[index + 2] = frame[index + 1];
        crc_input[index + 3] = frame[index];
    }
    crc = xz_mixer_crc16_x25(crc_input, sizeof(crc_input));

    /* The guest reads the CRC as a little-endian word then shifts it >> 16. */
    frame[54] = crc & 0xff;
    frame[55] = crc >> 8;
}

static void imx6_xz_mixer_commit_staged_frame(IMX6XZMixerDiagState *s)
{
    if (!s->staged_frame_pending) {
        return;
    }

    memcpy(s->frame, s->staged_frame, sizeof(s->frame));
    s->staged_frame_pending = false;
}

/*
 * QOM controls may change while the Linux SPI driver is reading a report.
 * The RBP CRC covers the entire 56-byte reply, so retain the active bytes and
 * cursor until the next report boundary rather than resetting mid-response.
 */
static void imx6_xz_mixer_stage_frame(IMX6XZMixerDiagState *s)
{
    imx6_xz_mixer_build_frame(s, s->staged_frame);
    s->staged_frame_pending = true;
    if (s->response_index == 0) {
        imx6_xz_mixer_commit_staged_frame(s);
    }
}

void imx6_xz_mixer_diag_set_ready(IMX6XZMixerDiagState *s, bool asserted)
{
    s->ready_asserted = asserted;
    /* The physical callback is get_rdy5() == !gpio_get_value(GPIO1_12). */
    if (s->ready) {
        qemu_set_irq(s->ready, asserted ? 0 : 1);
    }
}

static uint32_t imx6_xz_mixer_transfer(SSIPeripheral *peripheral,
                                        uint32_t value)
{
    IMX6XZMixerDiagState *s = IMX6_XZ_MIXER_DIAG(peripheral);
    uint8_t result;
    size_t word_base;
    size_t word_offset;

    if (!s->ready_asserted) {
        return 0xff;
    }

    if (s->response_index == 0) {
        imx6_xz_mixer_commit_staged_frame(s);
    }

    /*
     * ECSPI collects each wire word MSB-first then the ARM driver stores the
     * received u32 in native little-endian order.  ``frame`` is deliberately
     * RBP-facing (payload byte 19 and the CRC word are defined in that
     * in-memory layout), so serialize every 32-bit word in reverse order.
     *
     * This is a transport-order correction only.  It leaves the 56-byte
     * report construction and CRC calculation untouched.
     */
    word_base = s->response_index & ~(size_t)3;
    word_offset = s->response_index & 3;
    result = s->frame[word_base + (3 - word_offset)];
    s->transfer_count++;
    s->response_index++;
    if (s->response_index == IMX6_XZ_MIXER_FRAME_BYTES) {
        s->response_index = 0;
        s->completed_frame_count++;
    }
    return result;
}

/* SSI's base realize path invokes the peripheral-class callback unconditionally. */
static void imx6_xz_mixer_realize(SSIPeripheral *peripheral, Error **errp)
{
    /* This diagnostic responder has no resources beyond board GPIO wiring. */
}

static void imx6_xz_mixer_reset(DeviceState *dev)
{
    IMX6XZMixerDiagState *s = IMX6_XZ_MIXER_DIAG(dev);

    s->menu_high = false;
    s->head_buttons = 0;
    s->navigation_buttons = 0;
    s->rotary_buttons = 0;
    s->rotary_count = 0;
    s->transfer_count = 0;
    s->completed_frame_count = 0;
    imx6_xz_mixer_build_frame(s, s->frame);
    s->staged_frame_pending = false;
    s->response_index = 0;
    /* The model only exists in synthetic-menu mode, where a frame is ready. */
    imx6_xz_mixer_diag_set_ready(s, true);
}

static bool imx6_xz_mixer_get_menu_high(Object *obj, Error **errp)
{
    return IMX6_XZ_MIXER_DIAG(obj)->menu_high;
}

static void imx6_xz_mixer_set_menu_high(Object *obj, bool value,
                                         Error **errp)
{
    IMX6XZMixerDiagState *s = IMX6_XZ_MIXER_DIAG(obj);

    s->menu_high = value;
    imx6_xz_mixer_stage_frame(s);
}

/*
 * Each property is a held level, not a synthesized RBP event.  A host button
 * client must qom-set true on press and false on release, allowing the
 * original SPI driver, CRC checker, edge decoder, and key manager to retain
 * ownership of the transition.
 */
#define XZ_MIXER_BUTTONS(X) \
    X(source_rekordbox, "source-rekordbox-high", head_buttons, 0x10) \
    X(source_usb1, "source-usb1-high", head_buttons, 0x40) \
    X(source_usb2, "source-usb2-high", head_buttons, 0x80) \
    X(source_link, "source-link-high", head_buttons, 0x20) \
    X(browse, "browse-high", head_buttons, 0x01) \
    X(tag_list, "tag-list-high", head_buttons, 0x02) \
    X(info, "info-high", head_buttons, 0x04) \
    X(back, "back-high", navigation_buttons, 0x01) \
    X(rotary_push, "rotary-push-high", rotary_buttons, 0x01) \
    X(load_deck1, "load-deck1-high", navigation_buttons, 0x10) \
    X(load_deck2, "load-deck2-high", navigation_buttons, 0x20) \
    X(shortcut, "shortcut-high", navigation_buttons, 0x08) \
    X(tag_track, "tag-track-high", navigation_buttons, 0x02) \
    X(track_filter, "track-filter-high", navigation_buttons, 0x04) \
    X(time_mode, "time-mode-high", navigation_buttons, 0x40) \
    X(deck_quantize, "deck-quantize-high", navigation_buttons, 0x80)

#define DEFINE_XZ_MIXER_BUTTON(name, property, member, mask) \
static bool imx6_xz_mixer_get_##name(Object *obj, Error **errp) \
{ \
    return (IMX6_XZ_MIXER_DIAG(obj)->member & (mask)) != 0; \
} \
static void imx6_xz_mixer_set_##name(Object *obj, bool value, Error **errp) \
{ \
    IMX6XZMixerDiagState *s = IMX6_XZ_MIXER_DIAG(obj); \
    if (value) { \
        s->member |= (mask); \
    } else { \
        s->member &= ~(mask); \
    } \
    imx6_xz_mixer_stage_frame(s); \
}
XZ_MIXER_BUTTONS(DEFINE_XZ_MIXER_BUTTON)
#undef DEFINE_XZ_MIXER_BUTTON

static void imx6_xz_mixer_get_rotary_count(Object *obj, Visitor *v,
                                             const char *name, void *opaque,
                                             Error **errp)
{
    uint16_t value = IMX6_XZ_MIXER_DIAG(obj)->rotary_count;

    visit_type_uint16(v, name, &value, errp);
}

static void imx6_xz_mixer_set_rotary_count(Object *obj, Visitor *v,
                                             const char *name, void *opaque,
                                             Error **errp)
{
    IMX6XZMixerDiagState *s = IMX6_XZ_MIXER_DIAG(obj);
    uint16_t value;

    if (!visit_type_uint16(v, name, &value, errp)) {
        return;
    }
    s->rotary_count = value;
    imx6_xz_mixer_stage_frame(s);
}

static const VMStateDescription vmstate_imx6_xz_mixer_diag = {
    .name = TYPE_IMX6_XZ_MIXER_DIAG,
    .version_id = 5,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_SSI_PERIPHERAL(parent_obj, IMX6XZMixerDiagState),
        VMSTATE_BOOL(ready_asserted, IMX6XZMixerDiagState),
        VMSTATE_BOOL(menu_high, IMX6XZMixerDiagState),
        VMSTATE_UINT8_V(head_buttons, IMX6XZMixerDiagState, 2),
        VMSTATE_UINT8_V(navigation_buttons, IMX6XZMixerDiagState, 2),
        VMSTATE_UINT8_V(rotary_buttons, IMX6XZMixerDiagState, 2),
        VMSTATE_UINT16_V(rotary_count, IMX6XZMixerDiagState, 3),
        VMSTATE_UINT64_V(transfer_count, IMX6XZMixerDiagState, 4),
        VMSTATE_UINT64_V(completed_frame_count, IMX6XZMixerDiagState, 4),
        VMSTATE_UINT8_ARRAY(frame, IMX6XZMixerDiagState,
                            IMX6_XZ_MIXER_FRAME_BYTES),
        VMSTATE_UINT8_ARRAY_V(staged_frame, IMX6XZMixerDiagState,
                              IMX6_XZ_MIXER_FRAME_BYTES, 5),
        VMSTATE_BOOL_V(staged_frame_pending, IMX6XZMixerDiagState, 5),
        VMSTATE_UINT8(response_index, IMX6XZMixerDiagState),
        VMSTATE_END_OF_LIST()
    },
};

static void imx6_xz_mixer_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    SSIPeripheralClass *sc = SSI_PERIPHERAL_CLASS(oc);

    device_class_set_legacy_reset(dc, imx6_xz_mixer_reset);
    dc->vmsd = &vmstate_imx6_xz_mixer_diag;
    dc->desc = "Opt-in synthetic XDJ-XZ mixer-ready MENU diagnostic responder";
    sc->realize = imx6_xz_mixer_realize;
    sc->transfer = imx6_xz_mixer_transfer;
    sc->cs_polarity = SSI_CS_LOW;
}

static void imx6_xz_mixer_init(Object *obj)
{
    IMX6XZMixerDiagState *s = IMX6_XZ_MIXER_DIAG(obj);

    qdev_init_gpio_out_named(DEVICE(s), &s->ready, "ready", 1);
    object_property_add_bool(obj, "menu-high", imx6_xz_mixer_get_menu_high,
                             imx6_xz_mixer_set_menu_high);
#define ADD_XZ_MIXER_BUTTON(name, property, member, mask) \
    object_property_add_bool(obj, property, imx6_xz_mixer_get_##name, \
                             imx6_xz_mixer_set_##name);
    XZ_MIXER_BUTTONS(ADD_XZ_MIXER_BUTTON)
#undef ADD_XZ_MIXER_BUTTON
    object_property_add(obj, "rotary-count", "uint16",
                        imx6_xz_mixer_get_rotary_count,
                        imx6_xz_mixer_set_rotary_count, NULL, NULL);
    object_property_add_uint64_ptr(obj, "transfer-count", &s->transfer_count,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "completed-frame-count",
                                   &s->completed_frame_count,
                                   OBJ_PROP_FLAG_READ);
    imx6_xz_mixer_build_frame(s, s->frame);
}

static const TypeInfo imx6_xz_mixer_type_info = {
    .name = TYPE_IMX6_XZ_MIXER_DIAG,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(IMX6XZMixerDiagState),
    .instance_init = imx6_xz_mixer_init,
    .class_init = imx6_xz_mixer_class_init,
};

static void imx6_xz_mixer_register_types(void)
{
    type_register_static(&imx6_xz_mixer_type_info);
}

type_init(imx6_xz_mixer_register_types)
