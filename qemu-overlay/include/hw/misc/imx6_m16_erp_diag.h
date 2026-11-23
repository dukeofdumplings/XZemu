/*
 * Opt-in ERP updater diagnostic peripheral for the XDJ-XZ recovery path.
 *
 * This is neither an M16C model nor a representation of a physical unit. It
 * recognizes one source-derived, fixed-size updater handshake on ECSPI4 CS0
 * and exposes explicitly selected negative/diagnostic reply modes.
 */
#ifndef HW_MISC_IMX6_M16_ERP_DIAG_H
#define HW_MISC_IMX6_M16_ERP_DIAG_H

#include "hw/core/irq.h"
#include "hw/ssi/ssi.h"

#define TYPE_IMX6_M16_ERP_DIAG "imx6-m16-erp-diag"
OBJECT_DECLARE_SIMPLE_TYPE(IMX6M16ERPDiagState, IMX6_M16_ERP_DIAG)

#define IMX6_M16_ERP_REQUEST_BYTES 116
#define IMX6_M16_ERP_RESPONSE_BYTES 16

struct IMX6M16ERPDiagState {
    SSIPeripheral parent_obj;
    qemu_irq ready;
    char *mode;
    char *panel_kind;
    uint8_t request[IMX6_M16_ERP_REQUEST_BYTES];
    uint8_t request_length;
    uint8_t response[IMX6_M16_ERP_RESPONSE_BYTES];
    uint8_t response_index;
    bool request_low;
    bool ready_asserted;
    bool recognized_request;
    bool response_phase;
    uint8_t panel_frame[36];
    uint8_t panel_frame_bytes;
    uint8_t panel_response_index;
    /* Read-only transport evidence for the synthetic panel diagnostic. */
    uint64_t panel_transfer_count;
    uint64_t panel_complete_report_count;
    /*
     * Source-derived RBP ERP tempo fixture.  These fields are used only by
     * panel-first-reports-tempo-center on ECSPI4 CS0.  QMP changes remain
     * pending until a complete 36-byte ERP reply boundary, so a guest never
     * observes a partly rewritten CRC-protected frame.  The first reply is
     * deliberately the legacy CRC-valid bootstrap frame; the centred fixture
     * begins only after that known-good RBP panel handshake completes.
     */
    uint16_t tempo_position[2];
    uint16_t tempo_reference[2];
    uint16_t tempo_pending_position[2];
    uint16_t tempo_pending_reference[2];
    bool tempo_range[2];
    bool tempo_master[2];
    bool tempo_reset[2];
    bool tempo_pending_range[2];
    bool tempo_pending_master[2];
    bool tempo_pending_reset[2];
    /* Source-mapped per-deck RBP button bytes, staged per full ERP report. */
    uint8_t deck_button_bytes[2][12];
    uint8_t deck_pending_button_bytes[2][12];
    bool tempo_pending_dirty;
    bool tempo_initial_legacy_frame;
};

#endif /* HW_MISC_IMX6_M16_ERP_DIAG_H */
