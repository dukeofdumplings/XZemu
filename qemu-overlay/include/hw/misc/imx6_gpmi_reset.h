/*
 * i.MX6 GPMI-family reset-register model.
 *
 * This intentionally implements only the MXS SET/CLR/TOG reset semantics
 * used before XDJ-XZ U-Boot starts APBH DMA. It is not a NAND, DMA or BCH
 * model.
 */
#ifndef HW_MISC_IMX6_GPMI_RESET_H
#define HW_MISC_IMX6_GPMI_RESET_H

#include "hw/core/sysbus.h"
#include "hw/misc/imx6_macronix_nand.h"
#include "qom/object.h"

#define TYPE_IMX6_GPMI_RESET "imx6-gpmi-reset"
OBJECT_DECLARE_SIMPLE_TYPE(IMX6GPMIResetState, IMX6_GPMI_RESET)

#define IMX6_GPMI_RESET_SIZE 0x2000
#define IMX6_NAND_DIAG_APBH_REGS (0x2000 / sizeof(uint32_t))
#define IMX6_NAND_DIAG_BCH_REGS  (0x4000 / sizeof(uint32_t))

/* Opt-in, aggregate-only latency diagnostics. No per-access logging. */
#define IMX6_NAND_DIAG_PC_SLOTS 4
typedef enum IMX6NANDDiagnosticBlock {
    IMX6_NAND_DIAG_APBH,
    IMX6_NAND_DIAG_GPMI,
    IMX6_NAND_DIAG_BCH,
} IMX6NANDDiagnosticBlock;

typedef struct IMX6NANDDiagnosticRegister {
    uint64_t reads[4];
    uint64_t writes[4];
    uint32_t representative_pc[IMX6_NAND_DIAG_PC_SLOTS];
    uint64_t representative_pc_count[IMX6_NAND_DIAG_PC_SLOTS];
} IMX6NANDDiagnosticRegister;

typedef struct IMX6NANDDiagnostic {
    bool enabled;
    bool kernel_phase;
    IMX6NANDDiagnosticRegister apbh[IMX6_NAND_DIAG_APBH_REGS];
    IMX6NANDDiagnosticRegister gpmi[IMX6_GPMI_RESET_SIZE / sizeof(uint32_t)];
    IMX6NANDDiagnosticRegister bch[IMX6_NAND_DIAG_BCH_REGS];
    uint64_t apbh_descriptor_starts;
    uint64_t apbh_completions;
    uint64_t apbh_cancellations;
    uint64_t bch_completions;
    uint64_t nand_transactions;
    uint64_t nand_successful_deliveries;
} IMX6NANDDiagnostic;

struct IMX6GPMIResetState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t regs[IMX6_GPMI_RESET_SIZE / sizeof(uint32_t)];
    IMX6MacronixNANDState nand;
    IMX6MacronixNANDMedia media;
    IMX6MacronixNANDVirtualChip virtual_chip;
    IMX6MacronixNANDLogicalMedia logical_media;
    IMX6MacronixNANDLogicalMedia logical_kernel_media;
    IMX6MacronixNANDLogicalMedia logical_rootfs_media;
    IMX6MacronixNANDLogicalMedia logical_pdj_media;
    IMX6MacronixNANDLogicalMedia logical_settings_media;
    IMX6MacronixNANDLogicalOOBView logical_oob_view;
    IMX6MacronixNANDUBootMarkerView uboot_marker_view;
    char *nand_media_path;
    char *nand_media_coverage_path;
    char *virtual_nand_path;
    char *logical_nand_media_path;
    char *logical_nand_kernel_media_path;
    char *logical_nand_rootfs_media_path;
    char *logical_nand_pdj_media_path;
    char *logical_nand_settings_media_path;
    char *logical_nand_marker_map_path;
    char *logical_oob_view_samples_path;
    char *logical_oob_view_index_path;
    char *uboot_marker_view_map_path;
    uint32_t logical_nand_start_page;
    uint32_t logical_nand_kernel_start_page;
    uint32_t logical_nand_rootfs_start_page;
    uint32_t logical_nand_pdj_start_page;
    uint32_t logical_nand_settings_start_page;
    bool logical_nand_marker_projection;
    bool logical_oob_view_enabled;
    bool uboot_marker_view_enabled;
    /* P13-W: opt-in, volatile, MTD6-only main-area COW; never source media. */
    bool logical_nand_settings_cow;
    uint8_t *settings_cow_pages;
    bool settings_cow_materialized[5120];
    uint8_t settings_cow_program_count[5120];
    uint8_t settings_cow_staged[2048];
    bool settings_cow_staged_valid;
    uint32_t settings_cow_staged_page;
    bool diagnostic_counters_enabled;
    IMX6NANDDiagnostic diagnostic;
    /* Test-only packed per-512-byte-chunk fault counts for logical media. */
    uint32_t logical_nand_test_bitflips;
    /* Snapshot identity; the FILE handle and host identity are never moved. */
    bool snapshot_media_configured;
    uint64_t snapshot_media_size;
    uint64_t snapshot_media_device;
    uint64_t snapshot_media_inode;
    int64_t snapshot_media_mtime_sec;
    int64_t snapshot_media_mtime_nsec;
    bool snapshot_coverage_configured;
    uint64_t snapshot_coverage_size;
    uint64_t snapshot_coverage_device;
    uint64_t snapshot_coverage_inode;
    int64_t snapshot_coverage_mtime_sec;
    int64_t snapshot_coverage_mtime_nsec;
    bool page_fixture_enabled;
    bool virtual_nand_ram_cache;
    uint8_t virtual_nand_staged[IMX6_MACRONIX_PAGE_MAIN_SIZE];
    uint8_t virtual_nand_staged_auxiliary0;
    bool virtual_nand_staged_valid;
    uint32_t virtual_nand_staged_page;
    bool snapshot_virtual_chip_configured;
    uint64_t snapshot_virtual_chip_size;
    uint64_t snapshot_virtual_chip_device;
    uint64_t snapshot_virtual_chip_inode;
    int64_t snapshot_virtual_chip_mtime_sec;
    int64_t snapshot_virtual_chip_mtime_nsec;
    bool snapshot_logical_configured;
    uint64_t snapshot_logical_size;
    uint64_t snapshot_logical_device;
    uint64_t snapshot_logical_inode;
    int64_t snapshot_logical_mtime_sec;
    int64_t snapshot_logical_mtime_nsec;
    uint32_t snapshot_logical_start_page;
    uint32_t snapshot_logical_page_count;
    bool snapshot_logical_marker_configured;
    uint64_t snapshot_logical_marker_size;
    uint64_t snapshot_logical_marker_device;
    uint64_t snapshot_logical_marker_inode;
    int64_t snapshot_logical_marker_mtime_sec;
    int64_t snapshot_logical_marker_mtime_nsec;
    bool snapshot_logical_kernel_configured;
    uint64_t snapshot_logical_kernel_size;
    uint64_t snapshot_logical_kernel_device;
    uint64_t snapshot_logical_kernel_inode;
    int64_t snapshot_logical_kernel_mtime_sec;
    int64_t snapshot_logical_kernel_mtime_nsec;
    uint32_t snapshot_logical_kernel_start_page;
    uint32_t snapshot_logical_kernel_page_count;
    bool snapshot_logical_rootfs_configured;
    uint64_t snapshot_logical_rootfs_size;
    uint64_t snapshot_logical_rootfs_device;
    uint64_t snapshot_logical_rootfs_inode;
    int64_t snapshot_logical_rootfs_mtime_sec;
    int64_t snapshot_logical_rootfs_mtime_nsec;
    uint32_t snapshot_logical_rootfs_start_page;
    uint32_t snapshot_logical_rootfs_page_count;
    bool snapshot_logical_pdj_configured;
    uint64_t snapshot_logical_pdj_size;
    uint64_t snapshot_logical_pdj_device;
    uint64_t snapshot_logical_pdj_inode;
    int64_t snapshot_logical_pdj_mtime_sec;
    int64_t snapshot_logical_pdj_mtime_nsec;
    uint32_t snapshot_logical_pdj_start_page;
    uint32_t snapshot_logical_pdj_page_count;
    bool snapshot_logical_settings_configured;
    uint64_t snapshot_logical_settings_size;
    uint64_t snapshot_logical_settings_device;
    uint64_t snapshot_logical_settings_inode;
    int64_t snapshot_logical_settings_mtime_sec;
    int64_t snapshot_logical_settings_mtime_nsec;
    uint32_t snapshot_logical_settings_start_page;
    uint32_t snapshot_logical_settings_page_count;
    bool snapshot_logical_oob_configured;
    uint64_t snapshot_logical_oob_samples_size;
    uint64_t snapshot_logical_oob_samples_device;
    uint64_t snapshot_logical_oob_samples_inode;
    int64_t snapshot_logical_oob_samples_mtime_sec;
    int64_t snapshot_logical_oob_samples_mtime_nsec;
    uint64_t snapshot_logical_oob_index_size;
    uint64_t snapshot_logical_oob_index_device;
    uint64_t snapshot_logical_oob_index_inode;
    int64_t snapshot_logical_oob_index_mtime_sec;
    int64_t snapshot_logical_oob_index_mtime_nsec;
    bool snapshot_uboot_marker_configured;
    uint64_t snapshot_uboot_marker_size;
    uint64_t snapshot_uboot_marker_device;
    uint64_t snapshot_uboot_marker_inode;
    int64_t snapshot_uboot_marker_mtime_sec;
    int64_t snapshot_uboot_marker_mtime_nsec;
};

bool imx6_gpmi_nand_write(IMX6GPMIResetState *s, uint32_t phase,
                          uint32_t chip_select, uint8_t byte);
bool imx6_gpmi_nand_read(IMX6GPMIResetState *s, uint32_t chip_select,
                         uint8_t *bytes, uint32_t length);
bool imx6_gpmi_nand_ready(IMX6GPMIResetState *s, uint32_t chip_select);
int64_t imx6_gpmi_nand_ready_deadline_ns(const IMX6GPMIResetState *s);
bool imx6_gpmi_nand_page_media_unavailable(const IMX6GPMIResetState *s);
bool imx6_gpmi_nand_has_logical_media(const IMX6GPMIResetState *s);
const char *imx6_gpmi_nand_page_media_stop_reason(const IMX6GPMIResetState *s);
bool imx6_gpmi_nand_logical_read(IMX6GPMIResetState *s,
                                 uint8_t payload[IMX6_MACRONIX_PAGE_MAIN_SIZE],
                                 uint8_t auxiliary[16], bool *uncorrectable);
bool imx6_gpmi_nand_stage_program(IMX6GPMIResetState *s,
                                  const uint8_t payload[2048],
                                  uint8_t auxiliary0);
void imx6_gpmi_diagnostic_access(IMX6GPMIResetState *s,
                                 IMX6NANDDiagnosticBlock block,
                                 hwaddr offset, unsigned size, bool write);
void imx6_gpmi_diagnostic_apbh_descriptor(IMX6GPMIResetState *s);
void imx6_gpmi_diagnostic_apbh_completion(IMX6GPMIResetState *s);
void imx6_gpmi_diagnostic_apbh_cancellation(IMX6GPMIResetState *s);
void imx6_gpmi_diagnostic_bch_completion(IMX6GPMIResetState *s);

#endif
