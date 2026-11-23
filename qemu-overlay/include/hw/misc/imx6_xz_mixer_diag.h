/*
 * Opt-in, diagnostic-only XDJ-XZ mixer-ready responder.
 *
 * This transports a deliberately synthetic 56-byte mixer-ready frame through
 * the original ECSPI5/GPIO1_12 kernel path.  PNL is a separate 20-byte
 * ECSPI4-CS1 protocol and is not modeled here.  This is not a model of the
 * physical mixer MCU and is never attached by the default sabrelite machine.
 */
#ifndef HW_MISC_IMX6_XZ_MIXER_DIAG_H
#define HW_MISC_IMX6_XZ_MIXER_DIAG_H

#include "hw/core/irq.h"
#include "hw/ssi/ssi.h"

#define TYPE_IMX6_XZ_MIXER_DIAG "imx6-xz-mixer-diag"
OBJECT_DECLARE_SIMPLE_TYPE(IMX6XZMixerDiagState, IMX6_XZ_MIXER_DIAG)

#define IMX6_XZ_MIXER_FRAME_BYTES 56
#define IMX6_XZ_MIXER_PAYLOAD_BYTES 52

struct IMX6XZMixerDiagState {
    SSIPeripheral parent_obj;

    /* Active-low board signal: 0 means that an RX frame is available. */
    qemu_irq ready;
    bool ready_asserted;
    /* Byte 19 fields with independent QMP/QOM button state. */
    uint8_t head_buttons;
    /* Byte 18 fields with independent QMP/QOM button state. */
    uint8_t navigation_buttons;
    /* Byte 16 rotary-selector push field. */
    uint8_t rotary_buttons;
    /* Bytes 22-23: RBP-facing little-endian rotary-selector count. */
    uint16_t rotary_count;
    bool menu_high;
    /* Read-only QOM telemetry; it does not affect guest-visible delivery. */
    uint64_t transfer_count;
    uint64_t completed_frame_count;
    /* Guest-visible frame, immutable until a 56-byte SPI response ends. */
    uint8_t frame[IMX6_XZ_MIXER_FRAME_BYTES];
    /* Host QOM changes are staged here to prevent a torn guest report. */
    uint8_t staged_frame[IMX6_XZ_MIXER_FRAME_BYTES];
    bool staged_frame_pending;
    uint8_t response_index;
};

/* Called by the board after its GPIO1_12 wire has been connected. */
void imx6_xz_mixer_diag_set_ready(IMX6XZMixerDiagState *s, bool asserted);

#endif /* HW_MISC_IMX6_XZ_MIXER_DIAG_H */
