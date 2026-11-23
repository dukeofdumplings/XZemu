/*
 * Opt-in XDJ-XZ jog-controller diagnostic responder.
 *
 * This is deliberately limited to the RBP v1.26-decoded PLAY/CUE receive
 * fields.  It is not a replay of the physical jog MCU or its display path.
 */
#ifndef HW_MISC_IMX6_XZ_JOG_DIAG_H
#define HW_MISC_IMX6_XZ_JOG_DIAG_H

#include "hw/core/irq.h"
#include "hw/ssi/ssi.h"

#define TYPE_IMX6_XZ_JOG_DIAG "imx6-xz-jog-diag"
OBJECT_DECLARE_SIMPLE_TYPE(IMX6XZJogDiagState, IMX6_XZ_JOG_DIAG)

#define IMX6_XZ_JOG_TRANSFER_BYTES 148
#define IMX6_XZ_JOG_RX_BYTES 24
#define IMX6_XZ_JOG_CRC_BYTES 20

struct IMX6XZJogDiagState {
    SSIPeripheral parent_obj;

    qemu_irq ready;
    bool ready_asserted;
    /* The original JogCpuCom consumes an initial neutral report twice. */
    bool first_report_pending;
    bool play_high;
    bool cue_high;
    /* Bounded, opt-in transport telemetry; does not alter report delivery. */
    bool diagnostic_enable;
    /* Runtime gate for telemetry after a traced startup completes. */
    bool telemetry_enable;
    uint64_t transfer_count;
    uint64_t cs_assertion_count;
    uint64_t cs_deassertion_count;
    uint64_t active_transaction_bytes;
    uint64_t last_transaction_bytes;
    uint64_t completed_exchange_count;
    uint8_t diagnostic_exchange_logs;
    uint8_t frame[IMX6_XZ_JOG_TRANSFER_BYTES];
    uint8_t response_index;
};

void imx6_xz_jog_diag_set_ready(IMX6XZJogDiagState *s, bool asserted);

#endif /* HW_MISC_IMX6_XZ_JOG_DIAG_H */
