/*
 * XDJ-XZ ECSPI2/ECSPI3 jog-controller diagnostic responder.
 *
 * Exact RBP v1.26 disassembly establishes a 24-byte JogRxFrame: the first
 * twenty bytes are protected by common_crc16::crc after per-u32 byte reversal
 * and the resulting CRC occupies the high half of the final u32.  RBP reads
 * PLAY from byte 7 and CUE from byte 6, then emits the original key-manager
 * edges.  The enclosing exchange is 148 bytes.  Only those two receive bits
 * are modelled here; all other jog/display data remains zero.
 */
#include "qemu/osdep.h"
#include "hw/misc/imx6_xz_jog_diag.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/visitor.h"
#include "qemu/log.h"
#include "qemu/module.h"

static uint16_t xz_jog_crc16_x25(const uint8_t *bytes, size_t length)
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

static void imx6_xz_jog_prepare_frame(IMX6XZJogDiagState *s, bool neutral)
{
    uint8_t crc_input[IMX6_XZ_JOG_CRC_BYTES];
    uint16_t crc;
    size_t index;

    memset(s->frame, 0, sizeof(s->frame));
    /* RBP JogRxFrame offsets recovered from JogCpuCom::disassemble(). */
    s->frame[6] = neutral ? 0 : s->cue_high;
    s->frame[7] = neutral ? 0 : s->play_high;

    for (index = 0; index < sizeof(crc_input); index += 4) {
        crc_input[index] = s->frame[index + 3];
        crc_input[index + 1] = s->frame[index + 2];
        crc_input[index + 2] = s->frame[index + 1];
        crc_input[index + 3] = s->frame[index];
    }
    crc = xz_jog_crc16_x25(crc_input, sizeof(crc_input));
    /* JogCpuCom::calcCrc() returns crc << 16 on little-endian ARM. */
    s->frame[22] = crc & 0xff;
    s->frame[23] = crc >> 8;
}

void imx6_xz_jog_diag_set_ready(IMX6XZJogDiagState *s, bool asserted)
{
    s->ready_asserted = asserted;
    if (s->ready) {
        qemu_set_irq(s->ready, asserted ? 0 : 1);
    }
}

/* The 24-byte RX report begins at each manual ECSPI transaction. */
static int imx6_xz_jog_set_cs(SSIPeripheral *peripheral, bool cs)
{
    IMX6XZJogDiagState *s = IMX6_XZ_JOG_DIAG(peripheral);

    /*
     * The Pioneer subucom_spi_rdy driver uses one manual ECSPI transaction
     * per 148-byte exchange.  Transaction framing is functional behaviour:
     * it must not depend on whether optional diagnostics were enabled at
     * realize time.  In particular, a short/aborted exchange must not skew
     * the next report's CRC word.
     */
    if (!cs) { /* SSI_CS_LOW: assertion */
        s->active_transaction_bytes = 0;
        s->response_index = 0;
        imx6_xz_jog_prepare_frame(s, s->first_report_pending);
        if (s->telemetry_enable) {
            s->cs_assertion_count++;
        }
    } else {
        s->last_transaction_bytes = s->active_transaction_bytes;
        if (s->telemetry_enable) {
            s->cs_deassertion_count++;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "imx6-xz-jog: CS interval=%" PRIu64
                          " bytes, model-index=%u, exchanges=%" PRIu64 "\n",
                          s->last_transaction_bytes, s->response_index,
                          s->completed_exchange_count);
        }
    }
    return 0;
}

static uint32_t imx6_xz_jog_transfer(SSIPeripheral *peripheral,
                                      uint32_t value)
{
    IMX6XZJogDiagState *s = IMX6_XZ_JOG_DIAG(peripheral);
    size_t word_base;
    size_t word_offset;
    uint8_t result;

    if (!s->ready_asserted) {
        return 0xff;
    }
    if (s->telemetry_enable) {
        s->transfer_count++;
        s->active_transaction_bytes++;
    }

    word_base = s->response_index & ~(size_t)3;
    word_offset = s->response_index & 3;
    result = s->frame[word_base + (3 - word_offset)];
    s->response_index++;
    if (s->response_index == IMX6_XZ_JOG_RX_BYTES &&
        s->first_report_pending) {
        /* RBP double-decodes its first accepted report; keep it neutral. */
        s->first_report_pending = false;
        imx6_xz_jog_prepare_frame(s, false);
    }
    if (s->response_index == IMX6_XZ_JOG_TRANSFER_BYTES) {
        s->response_index = 0;
        if (s->telemetry_enable) {
            s->completed_exchange_count++;
            if (s->diagnostic_exchange_logs < 30) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "imx6-xz-jog: model 148-byte boundary #%"
                              PRIu64 ", CS bytes=%" PRIu64 "\n",
                              s->completed_exchange_count,
                              s->active_transaction_bytes);
                s->diagnostic_exchange_logs++;
            }
        }
        imx6_xz_jog_prepare_frame(s, false);
    }
    return result;
}

static void imx6_xz_jog_realize(SSIPeripheral *peripheral, Error **errp)
{
    IMX6XZJogDiagState *s = IMX6_XZ_JOG_DIAG(peripheral);

    /* The machine property selects initial telemetry; QMP may later stop it. */
    s->telemetry_enable = s->diagnostic_enable;
}

static void imx6_xz_jog_reset(DeviceState *dev)
{
    IMX6XZJogDiagState *s = IMX6_XZ_JOG_DIAG(dev);

    s->play_high = false;
    s->cue_high = false;
    s->first_report_pending = true;
    imx6_xz_jog_prepare_frame(s, true);
    s->response_index = 0;
    imx6_xz_jog_diag_set_ready(s, true);
}

#define DEFINE_JOG_BUTTON(name, property) \
static bool imx6_xz_jog_get_##name(Object *obj, Error **errp) \
{ \
    return IMX6_XZ_JOG_DIAG(obj)->name##_high; \
} \
static void imx6_xz_jog_set_##name(Object *obj, bool value, Error **errp) \
{ \
    IMX6XZJogDiagState *s = IMX6_XZ_JOG_DIAG(obj); \
    s->name##_high = value; \
    if (!s->first_report_pending && s->response_index == 0) { \
        imx6_xz_jog_prepare_frame(s, false); \
    } \
}
DEFINE_JOG_BUTTON(play, "play-high")
DEFINE_JOG_BUTTON(cue, "cue-high")
#undef DEFINE_JOG_BUTTON

static bool imx6_xz_jog_get_telemetry_enable(Object *obj, Error **errp)
{
    return IMX6_XZ_JOG_DIAG(obj)->telemetry_enable;
}

static void imx6_xz_jog_set_telemetry_enable(Object *obj, bool value,
                                              Error **errp)
{
    IMX6_XZ_JOG_DIAG(obj)->telemetry_enable = value;
}

static const VMStateDescription vmstate_imx6_xz_jog_diag = {
    .name = TYPE_IMX6_XZ_JOG_DIAG,
    .version_id = 4,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_SSI_PERIPHERAL(parent_obj, IMX6XZJogDiagState),
        VMSTATE_BOOL(ready_asserted, IMX6XZJogDiagState),
        VMSTATE_BOOL_V(first_report_pending, IMX6XZJogDiagState, 2),
        VMSTATE_BOOL_V(diagnostic_enable, IMX6XZJogDiagState, 3),
        VMSTATE_BOOL_V(telemetry_enable, IMX6XZJogDiagState, 4),
        VMSTATE_UINT64_V(transfer_count, IMX6XZJogDiagState, 3),
        VMSTATE_UINT64_V(cs_assertion_count, IMX6XZJogDiagState, 3),
        VMSTATE_UINT64_V(cs_deassertion_count, IMX6XZJogDiagState, 3),
        VMSTATE_UINT64_V(active_transaction_bytes, IMX6XZJogDiagState, 3),
        VMSTATE_UINT64_V(last_transaction_bytes, IMX6XZJogDiagState, 3),
        VMSTATE_UINT64_V(completed_exchange_count, IMX6XZJogDiagState, 3),
        VMSTATE_UINT8_V(diagnostic_exchange_logs, IMX6XZJogDiagState, 3),
        VMSTATE_BOOL(play_high, IMX6XZJogDiagState),
        VMSTATE_BOOL(cue_high, IMX6XZJogDiagState),
        VMSTATE_UINT8_ARRAY(frame, IMX6XZJogDiagState,
                            IMX6_XZ_JOG_TRANSFER_BYTES),
        VMSTATE_UINT8(response_index, IMX6XZJogDiagState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property imx6_xz_jog_diag_properties[] = {
    DEFINE_PROP_BOOL("diagnostic-enable", IMX6XZJogDiagState,
                     diagnostic_enable, false),
};

static void imx6_xz_jog_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    SSIPeripheralClass *sc = SSI_PERIPHERAL_CLASS(oc);

    device_class_set_legacy_reset(dc, imx6_xz_jog_reset);
    dc->vmsd = &vmstate_imx6_xz_jog_diag;
    device_class_set_props(dc, imx6_xz_jog_diag_properties);
    dc->desc = "Opt-in synthetic XDJ-XZ jog PLAY/CUE diagnostic responder";
    sc->realize = imx6_xz_jog_realize;
    sc->transfer = imx6_xz_jog_transfer;
    sc->set_cs = imx6_xz_jog_set_cs;
    sc->cs_polarity = SSI_CS_LOW;
}

static void imx6_xz_jog_init(Object *obj)
{
    IMX6XZJogDiagState *s = IMX6_XZ_JOG_DIAG(obj);

    qdev_init_gpio_out_named(DEVICE(s), &s->ready, "ready", 1);
    object_property_add_bool(obj, "play-high", imx6_xz_jog_get_play,
                             imx6_xz_jog_set_play);
    object_property_add_bool(obj, "cue-high", imx6_xz_jog_get_cue,
                             imx6_xz_jog_set_cue);
    object_property_add_bool(obj, "telemetry-enable",
                             imx6_xz_jog_get_telemetry_enable,
                             imx6_xz_jog_set_telemetry_enable);
    object_property_add_uint64_ptr(obj, "transfer-count", &s->transfer_count,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "cs-assertion-count",
                                   &s->cs_assertion_count, OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "cs-deassertion-count",
                                   &s->cs_deassertion_count, OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "active-transaction-bytes",
                                   &s->active_transaction_bytes,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "last-transaction-bytes",
                                   &s->last_transaction_bytes,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "completed-exchange-count",
                                   &s->completed_exchange_count,
                                   OBJ_PROP_FLAG_READ);
    s->first_report_pending = true;
    imx6_xz_jog_prepare_frame(s, true);
    s->response_index = 0;
}

static const TypeInfo imx6_xz_jog_type_info = {
    .name = TYPE_IMX6_XZ_JOG_DIAG,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(IMX6XZJogDiagState),
    .instance_init = imx6_xz_jog_init,
    .class_init = imx6_xz_jog_class_init,
};

static void imx6_xz_jog_register_types(void)
{
    type_register_static(&imx6_xz_jog_type_info);
}

type_init(imx6_xz_jog_register_types)
