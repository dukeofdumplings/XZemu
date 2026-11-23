/*
 * XDJ-XZ ERP updater and panel-first-report diagnostic responder.
 *
 * The original updater's M16SpiComm::enterUpdateMode_core() builds a 116-byte
 * request, reverses each 32-bit word in its little-endian memory buffer for
 * ECSPI's most-significant-byte-first serial transport, then applies the
 * GPIO handshake below. This model intentionally recognizes only that exact
 * request on an explicitly attached ECSPI4 CS0 bus. It is false by default at
 * the machine level and has no relationship to physical hardware.
 */
#include "qemu/osdep.h"
#include "hw/misc/imx6_m16_erp_diag.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define XZ_ERP_TEMPO_ADC_MIN       192u
#define XZ_ERP_TEMPO_ADC_CENTER   8192u
#define XZ_ERP_TEMPO_ADC_MAX     16176u

/* Reconstructed from updater-update SHA-256 20bb...c2b838a at 0x1455c-14600. */
static const uint8_t enter_update_request[IMX6_M16_ERP_REQUEST_BYTES] = {
    /*
     * The updater constructs the canonical packet with 55 at +110 and
     * its X-25 CRC (2c ae) at +112..113, then REV's every 32-bit word
     * immediately before write(2).  ECSPI emits the canonical byte order.
     */
    [110] = 0x55, [112] = 0x2c, [113] = 0xae,
};

static uint8_t reverse_byte(uint8_t value)
{
    value = (value >> 4) | (value << 4);
    value = ((value & 0xcc) >> 2) | ((value & 0x33) << 2);
    return ((value & 0xaa) >> 1) | ((value & 0x55) << 1);
}

/*
 * Source-derived reply validation: upd_crc_ccitt16_go8() bit-reverses every
 * byte through its table, then performs a CCITT recurrence from 0xffff.
 */
static uint16_t updater_reply_crc16(const uint8_t *bytes, size_t length)
{
    uint16_t crc = 0xffff;
    size_t index;

    for (index = 0; index < length; index++) {
        unsigned bit;

        crc ^= (uint16_t)reverse_byte(bytes[index]) << 8;
        for (bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
        }
    }
    return crc;
}

static void set_ready(IMX6M16ERPDiagState *s, bool level)
{
    s->ready_asserted = level;
    /* Device reset runs during realize, before board wiring attaches ready. */
    if (s->ready) {
        qemu_set_irq(s->ready, level);
    }
}

static bool is_mode(const IMX6M16ERPDiagState *s, const char *mode)
{
    return s->mode && !strcmp(s->mode, mode);
}

static bool is_panel_first_report(const IMX6M16ERPDiagState *s)
{
    return (is_mode(s, "panel-first-reports") ||
            is_mode(s, "panel-first-reports-tempo-center")) && s->panel_kind &&
           (!strcmp(s->panel_kind, "erp") || !strcmp(s->panel_kind, "pnl"));
}

static bool is_tempo_center_erp(const IMX6M16ERPDiagState *s)
{
    return is_mode(s, "panel-first-reports-tempo-center") && s->panel_kind &&
           !strcmp(s->panel_kind, "erp");
}

/* RBP PanelCommData::calulateCheckCrc(): per-word byte reversal, X-25. */
static uint16_t panel_crc16(const uint8_t *bytes, size_t length)
{
    uint16_t crc = 0xffff;
    size_t index;

    for (index = 0; index < length; index += 4) {
        int byte;

        for (byte = 3; byte >= 0; byte--) {
            unsigned bit;

            crc ^= bytes[index + byte];
            for (bit = 0; bit < 8; bit++) {
                crc = (crc >> 1) ^ ((crc & 1) ? 0x8408 : 0);
            }
        }
    }
    return crc ^ 0xffff;
}

static void apply_pending_tempo(IMX6M16ERPDiagState *s)
{
    unsigned deck;

    if (!s->tempo_pending_dirty) {
        return;
    }
    for (deck = 0; deck < ARRAY_SIZE(s->tempo_position); deck++) {
        s->tempo_position[deck] = s->tempo_pending_position[deck];
        s->tempo_reference[deck] = s->tempo_pending_reference[deck];
        s->tempo_range[deck] = s->tempo_pending_range[deck];
        s->tempo_master[deck] = s->tempo_pending_master[deck];
        s->tempo_reset[deck] = s->tempo_pending_reset[deck];
        memcpy(s->deck_button_bytes[deck],
               s->deck_pending_button_bytes[deck],
               sizeof(s->deck_button_bytes[deck]));
    }
    s->tempo_pending_dirty = false;
}

static void prepare_tempo_center_report(IMX6M16ERPDiagState *s)
{
    unsigned deck;

    for (deck = 0; deck < ARRAY_SIZE(s->tempo_position); deck++) {
        size_t base = deck * 12;

        stw_le_p(&s->panel_frame[base], s->tempo_position[deck]);
        stw_le_p(&s->panel_frame[base + 6], s->tempo_reference[deck]);
        if (s->tempo_master[deck]) {
            s->panel_frame[base + 10] |= 0x40;
        }
        if (s->tempo_range[deck]) {
            s->panel_frame[base + 11] |= 0x08;
        }
        if (s->tempo_reset[deck]) {
            s->panel_frame[base + 11] |= 0x80;
        }
        /*
         * The source-mapped buttons occupy distinct fields in the staged
         * 12-byte deck block.  Copy every button field after the analog and
         * tempo fixtures above, so a control at byte 10 (SYNC / MASTER) is
         * delivered in the same CRC-protected ERP reply as the existing pad
         * and loop controls.
         */
        for (unsigned byte = 0; byte < 12; byte++) {
            s->panel_frame[base + byte] |= s->deck_button_bytes[deck][byte];
        }
    }
}

static void prepare_panel_first_report(IMX6M16ERPDiagState *s)
{
    size_t prefix_bytes;
    size_t crc_offset;
    uint16_t crc;

    g_assert(is_panel_first_report(s));
    s->panel_frame_bytes = !strcmp(s->panel_kind, "erp") ? 36 : 20;
    prefix_bytes = s->panel_frame_bytes - 4;
    crc_offset = s->panel_frame_bytes - 2;
    memset(s->panel_frame, 0, sizeof(s->panel_frame));
    if (is_tempo_center_erp(s) && !s->tempo_initial_legacy_frame) {
        prepare_tempo_center_report(s);
    }
    crc = panel_crc16(s->panel_frame, prefix_bytes);
    s->panel_frame[crc_offset] = crc & 0xff;
    s->panel_frame[crc_offset + 1] = crc >> 8;
    s->panel_response_index = 0;
}

static void encode_bus_response(IMX6M16ERPDiagState *s,
                                const uint8_t parsed[16])
{
    /* ECSPI serializes MSB first and the user's read path reverses words. */
    memcpy(s->response, parsed, IMX6_M16_ERP_RESPONSE_BYTES);
}

static void prepare_response(IMX6M16ERPDiagState *s)
{
    uint8_t parsed[IMX6_M16_ERP_RESPONSE_BYTES] = { 0 };
    uint16_t crc;

    s->response_index = 0;
    memset(s->response, 0xff, sizeof(s->response));
    if (is_mode(s, "withhold-reply")) {
        return;
    }
    if (is_mode(s, "zero-reply")) {
        memset(s->response, 0, sizeof(s->response));
        return;
    }

    if (is_mode(s, "bad-header")) {
        parsed[0] = 0xfd;
    } else {
        parsed[0] = 0xfe;
    }
    /* byte one is the source-observed success status when header is accepted */
    parsed[1] = 0;
    crc = updater_reply_crc16(parsed, 12);
    parsed[12] = crc & 0xff;
    parsed[13] = crc >> 8;
    if (is_mode(s, "bad-crc")) {
        parsed[12] ^= 1;
    }
    encode_bus_response(s, parsed);
}

static void imx6_m16_erp_request(void *opaque, int line, int level)
{
    IMX6M16ERPDiagState *s = opaque;

    g_assert(line == 0);
    if (!level) {
        s->request_low = true;
        /*
         * enterUpdateMode_core() writes its 116 SPI bytes *before*
         * request_wait() lowers this line.  Do not discard those bytes here.
         */
        if (s->recognized_request &&
            !is_mode(s, "withhold-ready") &&
            !is_mode(s, "no-request")) {
            set_ready(s, true);
        }
        return;
    }

    if (s->request_low && s->ready_asserted) {
        /* request_wait() wrote HIGH after observing the ready input. */
        set_ready(s, false);
        s->response_phase = true;
        prepare_response(s);
    }
    s->request_low = false;
}

static uint32_t imx6_m16_erp_transfer(SSIPeripheral *peripheral,
                                       uint32_t value)
{
    IMX6M16ERPDiagState *s = IMX6_M16_ERP_DIAG(peripheral);
    uint8_t byte = value;

    if (is_panel_first_report(s)) {
        if (s->panel_response_index == 0 && is_tempo_center_erp(s) &&
            !s->tempo_initial_legacy_frame &&
            s->tempo_pending_dirty) {
            apply_pending_tempo(s);
            prepare_panel_first_report(s);
        }
        size_t word_base = s->panel_response_index & ~(size_t)3;
        size_t word_offset = s->panel_response_index & 3;
        uint8_t result = s->panel_frame[word_base + (3 - word_offset)];

        s->panel_transfer_count++;
        s->panel_response_index++;
        if (s->panel_response_index == s->panel_frame_bytes) {
            s->panel_response_index = 0;
            s->panel_complete_report_count++;
            if (is_tempo_center_erp(s)) {
                if (s->tempo_initial_legacy_frame) {
                    s->tempo_initial_legacy_frame = false;
                }
                if (s->tempo_pending_dirty) {
                    apply_pending_tempo(s);
                }
                prepare_panel_first_report(s);
            }
        }
        return result;
    }

    if (s->response_phase) {
        if (s->response_index < IMX6_M16_ERP_RESPONSE_BYTES) {
            return s->response[s->response_index++];
        }
        return 0xff;
    }

    /* A retry starts with a fresh 116-byte write while the line is low. */
    if (s->request_length == IMX6_M16_ERP_REQUEST_BYTES) {
        s->request_length = 0;
        s->recognized_request = false;
        set_ready(s, false);
    }

    if (s->request_length < IMX6_M16_ERP_REQUEST_BYTES) {
        s->request[s->request_length++] = byte;
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      TYPE_IMX6_M16_ERP_DIAG ": rejected SPI traffic after "
                      "the 116-byte request\n");
        return 0xff;
    }

    if (s->request_length == IMX6_M16_ERP_REQUEST_BYTES) {
        s->recognized_request = !memcmp(s->request, enter_update_request,
                                        sizeof(enter_update_request));
        if (!s->recognized_request) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          TYPE_IMX6_M16_ERP_DIAG ": rejected unexpected "
                          "116-byte ECSPI4-CS0 request\n");
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          TYPE_IMX6_M16_ERP_DIAG ": recognized source-derived "
                          "116-byte ERP request mode=%s request-low=%u\n",
                          s->mode, s->request_low);
            if (s->request_low && !is_mode(s, "withhold-ready") &&
                !is_mode(s, "no-request")) {
                set_ready(s, true);
            }
        }
    }
    return 0xff;
}

/*
 * ERP/PNL reports are complete SPI messages, not one continuous byte
 * stream.  The Pioneer spi_imx driver drops the internal chip select when a
 * message's FIFO drains; retain that boundary here so an earlier short
 * transfer cannot leave the next 20-byte PNL report CRC-shifted.
 */
static int imx6_m16_erp_set_cs(SSIPeripheral *peripheral, bool cs)
{
    IMX6M16ERPDiagState *s = IMX6_M16_ERP_DIAG(peripheral);

    if (is_panel_first_report(s) && !cs) {
        s->panel_response_index = 0;
    }
    return 0;
}

static void imx6_m16_erp_reset(DeviceState *dev)
{
    IMX6M16ERPDiagState *s = IMX6_M16_ERP_DIAG(dev);

    s->request_length = 0;
    s->response_index = 0;
    s->request_low = false;
    s->recognized_request = false;
    s->response_phase = false;
    s->panel_response_index = 0;
    s->panel_transfer_count = 0;
    s->panel_complete_report_count = 0;
    for (unsigned deck = 0; deck < ARRAY_SIZE(s->tempo_position); deck++) {
        s->tempo_position[deck] = XZ_ERP_TEMPO_ADC_CENTER;
        s->tempo_reference[deck] = XZ_ERP_TEMPO_ADC_CENTER;
        s->tempo_pending_position[deck] = XZ_ERP_TEMPO_ADC_CENTER;
        s->tempo_pending_reference[deck] = XZ_ERP_TEMPO_ADC_CENTER;
        s->tempo_range[deck] = false;
        s->tempo_master[deck] = false;
        s->tempo_reset[deck] = false;
        s->tempo_pending_range[deck] = false;
        s->tempo_pending_master[deck] = false;
        s->tempo_pending_reset[deck] = false;
        memset(s->deck_button_bytes[deck], 0,
               sizeof(s->deck_button_bytes[deck]));
        memset(s->deck_pending_button_bytes[deck], 0,
               sizeof(s->deck_pending_button_bytes[deck]));
    }
    s->tempo_pending_dirty = false;
    s->tempo_initial_legacy_frame = is_tempo_center_erp(s);
    if (is_panel_first_report(s)) {
        prepare_panel_first_report(s);
    }
    memset(s->request, 0, sizeof(s->request));
    memset(s->response, 0xff, sizeof(s->response));
    /*
     * Device reset is invoked by qdev_realize() before sabrelite.c connects
     * this optional output to GPIO1_16.  A qdev GPIO output slot exists at
     * that point, but has no sink callback yet, so do not drive it here.
     * GPIO reset itself establishes the deasserted input state; subsequent
     * protocol transitions use set_ready() after the board wiring is live.
     */
    s->ready_asserted = false;
}

/* SSI's base realize method invokes the peripheral-class callback. */
static void imx6_m16_erp_realize(SSIPeripheral *peripheral, Error **errp)
{
    /* This deliberately has no resources beyond its board-wired GPIOs. */
}

static const VMStateDescription vmstate_imx6_m16_erp_diag = {
    .name = TYPE_IMX6_M16_ERP_DIAG,
    .version_id = 5,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_SSI_PERIPHERAL(parent_obj, IMX6M16ERPDiagState),
        VMSTATE_UINT8_ARRAY(request, IMX6M16ERPDiagState,
                            IMX6_M16_ERP_REQUEST_BYTES),
        VMSTATE_UINT8(request_length, IMX6M16ERPDiagState),
        VMSTATE_UINT8_ARRAY(response, IMX6M16ERPDiagState,
                            IMX6_M16_ERP_RESPONSE_BYTES),
        VMSTATE_UINT8(response_index, IMX6M16ERPDiagState),
        VMSTATE_BOOL(request_low, IMX6M16ERPDiagState),
        VMSTATE_BOOL(ready_asserted, IMX6M16ERPDiagState),
        VMSTATE_BOOL(recognized_request, IMX6M16ERPDiagState),
        VMSTATE_BOOL(response_phase, IMX6M16ERPDiagState),
        VMSTATE_UINT8_ARRAY(panel_frame, IMX6M16ERPDiagState, 36),
        VMSTATE_UINT8(panel_frame_bytes, IMX6M16ERPDiagState),
        VMSTATE_UINT8(panel_response_index, IMX6M16ERPDiagState),
        VMSTATE_UINT64_V(panel_transfer_count, IMX6M16ERPDiagState, 4),
        VMSTATE_UINT64_V(panel_complete_report_count, IMX6M16ERPDiagState, 4),
        VMSTATE_UINT16_ARRAY(tempo_position, IMX6M16ERPDiagState, 2),
        VMSTATE_UINT16_ARRAY(tempo_reference, IMX6M16ERPDiagState, 2),
        VMSTATE_UINT16_ARRAY(tempo_pending_position, IMX6M16ERPDiagState, 2),
        VMSTATE_UINT16_ARRAY(tempo_pending_reference, IMX6M16ERPDiagState, 2),
        VMSTATE_BOOL_ARRAY(tempo_range, IMX6M16ERPDiagState, 2),
        VMSTATE_BOOL_ARRAY(tempo_master, IMX6M16ERPDiagState, 2),
        VMSTATE_BOOL_ARRAY(tempo_reset, IMX6M16ERPDiagState, 2),
        VMSTATE_BOOL_ARRAY(tempo_pending_range, IMX6M16ERPDiagState, 2),
        VMSTATE_BOOL_ARRAY(tempo_pending_master, IMX6M16ERPDiagState, 2),
        VMSTATE_BOOL_ARRAY(tempo_pending_reset, IMX6M16ERPDiagState, 2),
        VMSTATE_UINT8_2DARRAY_V(deck_button_bytes, IMX6M16ERPDiagState,
                                 2, 12, 5),
        VMSTATE_UINT8_2DARRAY_V(deck_pending_button_bytes,
                                 IMX6M16ERPDiagState, 2, 12, 5),
        VMSTATE_BOOL(tempo_pending_dirty, IMX6M16ERPDiagState),
        VMSTATE_BOOL_V(tempo_initial_legacy_frame, IMX6M16ERPDiagState, 3),
        VMSTATE_END_OF_LIST()
    },
};

typedef enum IMX6ERPTempoControl {
    IMX6_ERP_TEMPO_RANGE,
    IMX6_ERP_TEMPO_MASTER,
    IMX6_ERP_TEMPO_RESET,
} IMX6ERPTempoControl;

typedef struct IMX6ERPTempoProperty {
    uint8_t deck;
    IMX6ERPTempoControl control;
} IMX6ERPTempoProperty;

static const IMX6ERPTempoProperty tempo_button_properties[] = {
    { 0, IMX6_ERP_TEMPO_RANGE }, { 1, IMX6_ERP_TEMPO_RANGE },
    { 0, IMX6_ERP_TEMPO_MASTER }, { 1, IMX6_ERP_TEMPO_MASTER },
    { 0, IMX6_ERP_TEMPO_RESET }, { 1, IMX6_ERP_TEMPO_RESET },
};
static const uint8_t tempo_decks[] = { 0, 1 };

/* Exact RBP decoder fields, expressed in the staged 12-byte deck blocks. */
typedef struct IMX6ERPDeckButtonProperty {
    uint8_t deck;
    uint8_t byte;
    uint8_t mask;
} IMX6ERPDeckButtonProperty;

static const IMX6ERPDeckButtonProperty deck_button_properties[] = {
    /*
     * Pending validation: on the currently emitted ERP report, RBP observes
     * byte 10 bit 0x04 as REVERSE and bit 0x02 as SLIP.  Preserve that
     * observed behavior in the opt-in responder; do not treat it as a
     * physical-panel mapping until a raw reference frame verifies it.
     */
    /* deck 1: loop boundary, observed reverse/slip, memory cue controls, pads A-H */
    { 0, 11, 0x01 }, { 0, 11, 0x02 }, { 0, 10, 0x01 },
    { 0, 10, 0x04 }, { 0, 10, 0x02 },
    { 0, 11, 0x10 }, { 0, 11, 0x20 }, { 0, 11, 0x40 }, { 0, 10, 0x80 },
    { 0, 5, 0x01 },
    { 0, 4, 0x01 }, { 0, 4, 0x02 }, { 0, 4, 0x04 }, { 0, 4, 0x08 },
    { 0, 4, 0x10 }, { 0, 4, 0x20 }, { 0, 4, 0x40 }, { 0, 4, 0x80 },
    /* deck 1: source-mapped player synchronization controls */
    { 0, 10, 0x20 }, { 0, 10, 0x10 },
    /* deck 2: loop boundary, observed reverse/slip, memory cue controls, pads A-H */
    { 1, 11, 0x01 }, { 1, 11, 0x02 }, { 1, 10, 0x01 },
    { 1, 10, 0x04 }, { 1, 10, 0x02 },
    { 1, 11, 0x10 }, { 1, 11, 0x20 }, { 1, 11, 0x40 }, { 1, 10, 0x80 },
    { 1, 5, 0x01 },
    { 1, 4, 0x01 }, { 1, 4, 0x02 }, { 1, 4, 0x04 }, { 1, 4, 0x08 },
    { 1, 4, 0x10 }, { 1, 4, 0x20 }, { 1, 4, 0x40 }, { 1, 4, 0x80 },
    /* deck 2: source-mapped player synchronization controls */
    { 1, 10, 0x20 }, { 1, 10, 0x10 },
};

static void deck_button_get(Object *obj, Visitor *v, const char *name,
                            void *opaque, Error **errp)
{
    IMX6M16ERPDiagState *s = IMX6_M16_ERP_DIAG(obj);
    const IMX6ERPDeckButtonProperty *property = opaque;
    bool value = (s->deck_pending_button_bytes[property->deck][property->byte]
                  & property->mask) != 0;

    visit_type_bool(v, name, &value, errp);
}

static void deck_button_set(Object *obj, Visitor *v, const char *name,
                            void *opaque, Error **errp)
{
    IMX6M16ERPDiagState *s = IMX6_M16_ERP_DIAG(obj);
    const IMX6ERPDeckButtonProperty *property = opaque;
    bool value;

    if (!visit_type_bool(v, name, &value, errp)) {
        return;
    }
    if (value) {
        s->deck_pending_button_bytes[property->deck][property->byte] |=
            property->mask;
    } else {
        s->deck_pending_button_bytes[property->deck][property->byte] &=
            ~property->mask;
    }
    s->tempo_pending_dirty = true;
}

static void tempo_position_get(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    IMX6M16ERPDiagState *s = IMX6_M16_ERP_DIAG(obj);
    uint8_t deck = *(const uint8_t *)opaque;
    uint16_t value = s->tempo_pending_position[deck];

    visit_type_uint16(v, name, &value, errp);
}

static void tempo_position_set(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    IMX6M16ERPDiagState *s = IMX6_M16_ERP_DIAG(obj);
    uint8_t deck = *(const uint8_t *)opaque;
    uint16_t value;

    if (!visit_type_uint16(v, name, &value, errp)) {
        return;
    }
    if (value < XZ_ERP_TEMPO_ADC_MIN || value > XZ_ERP_TEMPO_ADC_MAX) {
        error_setg(errp, "tempo position must be between %u and %u",
                   XZ_ERP_TEMPO_ADC_MIN, XZ_ERP_TEMPO_ADC_MAX);
        return;
    }
    s->tempo_pending_position[deck] = value;
    s->tempo_pending_dirty = true;
}

static void tempo_reference_get(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    IMX6M16ERPDiagState *s = IMX6_M16_ERP_DIAG(obj);
    uint8_t deck = *(const uint8_t *)opaque;
    uint16_t value = s->tempo_pending_reference[deck];

    visit_type_uint16(v, name, &value, errp);
}

static void tempo_reference_set(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    IMX6M16ERPDiagState *s = IMX6_M16_ERP_DIAG(obj);
    uint8_t deck = *(const uint8_t *)opaque;
    uint16_t value;

    if (!visit_type_uint16(v, name, &value, errp)) {
        return;
    }
    if (value <= XZ_ERP_TEMPO_ADC_MIN || value >= XZ_ERP_TEMPO_ADC_MAX) {
        error_setg(errp, "tempo reference must be between %u and %u",
                   XZ_ERP_TEMPO_ADC_MIN + 1, XZ_ERP_TEMPO_ADC_MAX - 1);
        return;
    }
    s->tempo_pending_reference[deck] = value;
    s->tempo_pending_dirty = true;
}

static void tempo_button_get(Object *obj, Visitor *v, const char *name,
                             void *opaque, Error **errp)
{
    IMX6M16ERPDiagState *s = IMX6_M16_ERP_DIAG(obj);
    const IMX6ERPTempoProperty *property =
        opaque;
    bool value;

    switch (property->control) {
    case IMX6_ERP_TEMPO_RANGE:
        value = s->tempo_pending_range[property->deck];
        break;
    case IMX6_ERP_TEMPO_MASTER:
        value = s->tempo_pending_master[property->deck];
        break;
    case IMX6_ERP_TEMPO_RESET:
        value = s->tempo_pending_reset[property->deck];
        break;
    default:
        g_assert_not_reached();
    }
    visit_type_bool(v, name, &value, errp);
}

static void tempo_button_set(Object *obj, Visitor *v, const char *name,
                             void *opaque, Error **errp)
{
    IMX6M16ERPDiagState *s = IMX6_M16_ERP_DIAG(obj);
    const IMX6ERPTempoProperty *property = opaque;
    bool value;

    if (!visit_type_bool(v, name, &value, errp)) {
        return;
    }
    switch (property->control) {
    case IMX6_ERP_TEMPO_RANGE:
        s->tempo_pending_range[property->deck] = value;
        break;
    case IMX6_ERP_TEMPO_MASTER:
        s->tempo_pending_master[property->deck] = value;
        break;
    case IMX6_ERP_TEMPO_RESET:
        s->tempo_pending_reset[property->deck] = value;
        break;
    default:
        g_assert_not_reached();
    }
    s->tempo_pending_dirty = true;
}

static const Property imx6_m16_erp_properties[] = {
    DEFINE_PROP_STRING("mode", IMX6M16ERPDiagState, mode),
    DEFINE_PROP_STRING("panel-kind", IMX6M16ERPDiagState, panel_kind),
};

static void imx6_m16_erp_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    SSIPeripheralClass *sc = SSI_PERIPHERAL_CLASS(oc);

    device_class_set_legacy_reset(dc, imx6_m16_erp_reset);
    dc->vmsd = &vmstate_imx6_m16_erp_diag;
    dc->desc = "Opt-in XDJ-XZ ERP updater diagnostic responder";
    device_class_set_props(dc, imx6_m16_erp_properties);
    sc->realize = imx6_m16_erp_realize;
    sc->transfer = imx6_m16_erp_transfer;
    sc->set_cs = imx6_m16_erp_set_cs;
    sc->cs_polarity = SSI_CS_LOW;
}

static void imx6_m16_erp_init(Object *obj)
{
    IMX6M16ERPDiagState *s = IMX6_M16_ERP_DIAG(obj);

    s->mode = g_strdup("withhold-ready");
    qdev_init_gpio_out_named(DEVICE(s), &s->ready, "ready", 1);
    qdev_init_gpio_in_named(DEVICE(s), imx6_m16_erp_request, "request", 1);
    object_property_add(obj, "tempo-deck1-position", "uint16",
                        tempo_position_get, tempo_position_set,
                        NULL, (void *)&tempo_decks[0]);
    object_property_add(obj, "tempo-deck2-position", "uint16",
                        tempo_position_get, tempo_position_set,
                        NULL, (void *)&tempo_decks[1]);
    object_property_add(obj, "tempo-deck1-reference", "uint16",
                        tempo_reference_get, tempo_reference_set,
                        NULL, (void *)&tempo_decks[0]);
    object_property_add(obj, "tempo-deck2-reference", "uint16",
                        tempo_reference_get, tempo_reference_set,
                        NULL, (void *)&tempo_decks[1]);
    object_property_add(obj, "tempo-deck1-range-high", "bool",
                        tempo_button_get, tempo_button_set, NULL,
                        (void *)&tempo_button_properties[0]);
    object_property_add(obj, "tempo-deck2-range-high", "bool",
                        tempo_button_get, tempo_button_set, NULL,
                        (void *)&tempo_button_properties[1]);
    object_property_add(obj, "tempo-deck1-master-high", "bool",
                        tempo_button_get, tempo_button_set, NULL,
                        (void *)&tempo_button_properties[2]);
    object_property_add(obj, "tempo-deck2-master-high", "bool",
                        tempo_button_get, tempo_button_set, NULL,
                        (void *)&tempo_button_properties[3]);
    object_property_add(obj, "tempo-deck1-reset-high", "bool",
                        tempo_button_get, tempo_button_set, NULL,
                        (void *)&tempo_button_properties[4]);
    object_property_add(obj, "tempo-deck2-reset-high", "bool",
                        tempo_button_get, tempo_button_set, NULL,
                        (void *)&tempo_button_properties[5]);
    {
        static const char *const deck_button_names[] = {
            "deck1-loop-in-high", "deck1-loop-out-high",
            "deck1-reloop-exit-high", "deck1-reverse-high",
            "deck1-slip-high",
            "deck1-cue-loop-next-high", "deck1-cue-loop-prev-high",
            "deck1-memory-high", "deck1-delete-high",
            "deck1-hotcue-mode-high",
            "deck1-hotcue-a-high", "deck1-hotcue-b-high", "deck1-hotcue-c-high",
            "deck1-hotcue-d-high", "deck1-hotcue-e-high", "deck1-hotcue-f-high",
            "deck1-hotcue-g-high", "deck1-hotcue-h-high",
            "deck1-sync-high", "deck1-master-high",
            "deck2-loop-in-high", "deck2-loop-out-high",
            "deck2-reloop-exit-high", "deck2-reverse-high",
            "deck2-slip-high",
            "deck2-cue-loop-next-high", "deck2-cue-loop-prev-high",
            "deck2-memory-high", "deck2-delete-high",
            "deck2-hotcue-mode-high",
            "deck2-hotcue-a-high", "deck2-hotcue-b-high", "deck2-hotcue-c-high",
            "deck2-hotcue-d-high", "deck2-hotcue-e-high", "deck2-hotcue-f-high",
            "deck2-hotcue-g-high", "deck2-hotcue-h-high",
            "deck2-sync-high", "deck2-master-high",
        };
        unsigned index;

        g_assert(ARRAY_SIZE(deck_button_names) ==
                 ARRAY_SIZE(deck_button_properties));
        for (index = 0; index < ARRAY_SIZE(deck_button_names); index++) {
            object_property_add(obj, deck_button_names[index], "bool",
                                deck_button_get, deck_button_set, NULL,
                                (void *)&deck_button_properties[index]);
        }
    }
    object_property_add_uint64_ptr(obj, "panel-transfer-count",
                                   &s->panel_transfer_count,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "panel-complete-report-count",
                                   &s->panel_complete_report_count,
                                   OBJ_PROP_FLAG_READ);
}

static const TypeInfo imx6_m16_erp_type_info = {
    .name = TYPE_IMX6_M16_ERP_DIAG,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(IMX6M16ERPDiagState),
    .instance_init = imx6_m16_erp_init,
    .class_init = imx6_m16_erp_class_init,
};

static void imx6_m16_erp_register_types(void)
{
    type_register_static(&imx6_m16_erp_type_info);
}

type_init(imx6_m16_erp_register_types)
