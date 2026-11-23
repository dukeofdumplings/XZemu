/*
 * Minimal i.MX6 MXS-style reset register block.
 *
 * XDJ-XZ U-Boot's gpmi_reset_block() uses CTRL0, CTRL0_SET and CTRL0_CLR.
 * A soft reset latches CLKGATE; clearing it releases the block. Later NAND,
 * APBH-DMA and BCH behavior is deliberately not represented here.
 */
#include "qemu/osdep.h"
#include "hw/misc/imx6_gpmi_reset.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "target/arm/cpu.h"

#define CTRL0       0x000
#define CTRL0_SET   0x004
#define CTRL0_CLR   0x008
#define CTRL0_TOG   0x00c
#define GPMI_STAT   0x0b0
#define GPMI_STAT_READY_BUSY_CS0 (1U << 24)
#define MODULE_CLKGATE (1U << 30)
#define MODULE_SFTRST  (1U << 31)

#define GPMI_CTRL0_COMMAND_MODE_MASK 0x03000000
#define GPMI_CTRL0_COMMAND_MODE_WRITE 0x00000000
#define GPMI_CTRL0_COMMAND_MODE_READ 0x01000000
#define GPMI_CTRL0_CS_MASK 0x00700000
#define GPMI_CTRL0_CS_SHIFT 20
#define GPMI_CTRL0_ADDRESS_MASK 0x000e0000
#define GPMI_CTRL0_ADDRESS_SHIFT 17
#define GPMI_ADDRESS_DATA 0

#define XZ_ENV_FIRST_PAGE 2048
#define XZ_ENV_PAGE_COUNT 512
#define XZ_KERNEL_A_FIRST_PAGE 2560
#define XZ_KERNEL_A_PAGE_COUNT 3584
#define XZ_ROOTFS_FIRST_PAGE 12800
#define XZ_ROOTFS_PAGE_COUNT 10240 /* /init nanddump length: 0x1400000 */
#define XZ_PDJ_FIRST_PAGE 33280
#define XZ_PDJ_PAGE_COUNT 1536 /* /init nanddump length: 0x00300000 */
#define XZ_SETTINGS_FIRST_PAGE 28160
#define XZ_SETTINGS_PAGE_COUNT 5120 /* mtd6: exact 10 MiB UBI main-area */

static unsigned imx6_gpmi_diagnostic_width_index(unsigned size)
{
    switch (size) {
    case 1: return 0;
    case 2: return 1;
    case 4: return 2;
    case 8: return 3;
    default: return 4;
    }
}

static uint32_t imx6_gpmi_diagnostic_guest_pc(void)
{
    if (current_cpu && object_dynamic_cast(OBJECT(current_cpu), TYPE_ARM_CPU)) {
        return ARM_CPU(current_cpu)->env.regs[15];
    }
    return 0;
}

static void imx6_gpmi_diagnostic_note_pc(IMX6NANDDiagnosticRegister *reg,
                                         uint32_t pc)
{
    unsigned int i, lowest = 0;

    if (!pc) {
        return;
    }
    for (i = 0; i < IMX6_NAND_DIAG_PC_SLOTS; i++) {
        if (reg->representative_pc[i] == pc) {
            reg->representative_pc_count[i]++;
            return;
        }
        if (!reg->representative_pc[i]) {
            reg->representative_pc[i] = pc;
            reg->representative_pc_count[i] = 1;
            return;
        }
        if (reg->representative_pc_count[i] <
            reg->representative_pc_count[lowest]) {
            lowest = i;
        }
    }
    /* Bounded Space-Saving summary: retain likely frequent polling PCs. */
    reg->representative_pc[lowest] = pc;
    reg->representative_pc_count[lowest]++;
}

static IMX6NANDDiagnosticRegister *imx6_gpmi_diagnostic_register(
    IMX6GPMIResetState *s, IMX6NANDDiagnosticBlock block, hwaddr offset)
{
    switch (block) {
    case IMX6_NAND_DIAG_APBH:
        return offset < 0x2000 ? &s->diagnostic.apbh[offset / 4] : NULL;
    case IMX6_NAND_DIAG_GPMI:
        return offset < IMX6_GPMI_RESET_SIZE ? &s->diagnostic.gpmi[offset / 4] : NULL;
    case IMX6_NAND_DIAG_BCH:
        return offset < 0x4000 ? &s->diagnostic.bch[offset / 4] : NULL;
    }
    return NULL;
}

void imx6_gpmi_diagnostic_access(IMX6GPMIResetState *s,
                                 IMX6NANDDiagnosticBlock block,
                                 hwaddr offset, unsigned size, bool write)
{
    IMX6NANDDiagnosticRegister *reg;
    unsigned int width;

    if (!s || !s->diagnostic.enabled || offset & 3) {
        return;
    }
    width = imx6_gpmi_diagnostic_width_index(size);
    if (width == 4 || !(reg = imx6_gpmi_diagnostic_register(s, block, offset))) {
        return;
    }
    if (write) {
        reg->writes[width]++;
    } else {
        reg->reads[width]++;
        imx6_gpmi_diagnostic_note_pc(reg, imx6_gpmi_diagnostic_guest_pc());
    }
}

void imx6_gpmi_diagnostic_apbh_descriptor(IMX6GPMIResetState *s)
{
    if (s && s->diagnostic.enabled) {
        s->diagnostic.apbh_descriptor_starts++;
    }
}

void imx6_gpmi_diagnostic_apbh_completion(IMX6GPMIResetState *s)
{
    if (s && s->diagnostic.enabled) {
        s->diagnostic.apbh_completions++;
    }
}

void imx6_gpmi_diagnostic_apbh_cancellation(IMX6GPMIResetState *s)
{
    if (s && s->diagnostic.enabled) {
        s->diagnostic.apbh_cancellations++;
    }
}

void imx6_gpmi_diagnostic_bch_completion(IMX6GPMIResetState *s)
{
    if (s && s->diagnostic.enabled) {
        s->diagnostic.bch_completions++;
    }
}

static uint64_t imx6_gpmi_diagnostic_total(const IMX6NANDDiagnosticRegister *reg)
{
    return reg->reads[0] + reg->reads[1] + reg->reads[2] + reg->reads[3] +
           reg->writes[0] + reg->writes[1] + reg->writes[2] + reg->writes[3];
}

static void imx6_gpmi_diagnostic_dump_block(const char *name,
                                             IMX6NANDDiagnosticRegister *regs,
                                             unsigned int count)
{
    unsigned int rank;

    for (rank = 0; rank < 6; rank++) {
        unsigned int i, best = count;
        uint64_t best_total = 0;
        unsigned int pc_slot = 0;

        for (i = 0; i < count; i++) {
            uint64_t total = imx6_gpmi_diagnostic_total(&regs[i]);

            if (!total) {
                continue;
            }
            if (total > best_total) {
                best = i;
                best_total = total;
            }
        }
        if (best == count) {
            break;
        }
        for (i = 1; i < IMX6_NAND_DIAG_PC_SLOTS; i++) {
            if (regs[best].representative_pc_count[i] >
                regs[best].representative_pc_count[pc_slot]) {
                pc_slot = i;
            }
        }
        fprintf(stderr,
                "XZ_DIAG: block=%s offset=0x%03x r1=%" PRIu64
                " r2=%" PRIu64 " r4=%" PRIu64 " r8=%" PRIu64
                " w1=%" PRIu64 " w2=%" PRIu64 " w4=%" PRIu64
                " w8=%" PRIu64 " representative-read-pc=0x%08x pc-samples=%" PRIu64 "\n",
                name, best * 4, regs[best].reads[0], regs[best].reads[1],
                regs[best].reads[2], regs[best].reads[3],
                regs[best].writes[0], regs[best].writes[1],
                regs[best].writes[2], regs[best].writes[3],
                regs[best].representative_pc[pc_slot],
                regs[best].representative_pc_count[pc_slot]);
        /* Consume this selection; diagnostics are emitted only at phase end. */
        memset(&regs[best], 0, sizeof(regs[best]));
    }
}

static void imx6_gpmi_diagnostic_dump_phase(IMX6GPMIResetState *s)
{
    IMX6NANDDiagnostic *diag = &s->diagnostic;

    if (!diag->enabled) {
        return;
    }
    fprintf(stderr,
            "XZ_DIAG: phase=%s descriptors=%" PRIu64
            " completions=%" PRIu64 " cancellations=%" PRIu64
            " bch-completions=%" PRIu64 " nand-transactions=%" PRIu64
            " successful-deliveries=%" PRIu64 "\n",
            diag->kernel_phase ? "kernel-transfer" : "scan",
            diag->apbh_descriptor_starts, diag->apbh_completions,
            diag->apbh_cancellations, diag->bch_completions,
            diag->nand_transactions, diag->nand_successful_deliveries);
    imx6_gpmi_diagnostic_dump_block("apbh", diag->apbh,
                                    IMX6_NAND_DIAG_APBH_REGS);
    imx6_gpmi_diagnostic_dump_block("gpmi", diag->gpmi,
                                    IMX6_GPMI_RESET_SIZE / sizeof(uint32_t));
    imx6_gpmi_diagnostic_dump_block("bch", diag->bch,
                                    IMX6_NAND_DIAG_BCH_REGS);
}

static void imx6_gpmi_diagnostic_begin_kernel_phase(IMX6GPMIResetState *s)
{
    if (!s->diagnostic.enabled || s->diagnostic.kernel_phase) {
        return;
    }
    imx6_gpmi_diagnostic_dump_phase(s);
    memset(&s->diagnostic, 0, sizeof(s->diagnostic));
    s->diagnostic.enabled = true;
    s->diagnostic.kernel_phase = true;
}

static IMX6MacronixNANDLogicalMedia *imx6_gpmi_logical_media_for_page(
    IMX6GPMIResetState *s, uint32_t page)
{
    IMX6MacronixNANDLogicalMedia *regions[] = {
        &s->logical_media, &s->logical_kernel_media,
        &s->logical_rootfs_media, &s->logical_pdj_media,
        &s->logical_settings_media,
    };
    size_t i;

    for (i = 0; i < G_N_ELEMENTS(regions); i++) {
        IMX6MacronixNANDLogicalMedia *region = regions[i];

        if (region->file && page >= region->start_page &&
            page - region->start_page < region->page_count) {
            return region;
        }
    }
    return NULL;
}

static bool imx6_gpmi_virtual_chip_can_commit(const IMX6GPMIResetState *s)
{
    return s->virtual_chip.file && s->virtual_nand_staged_valid &&
           s->virtual_nand_staged_page == s->nand.page_number &&
           s->nand.phase == IMX6_MACRONIX_PAGE_PROGRAM_DATA &&
           s->nand.page_column == 0;
}

static bool imx6_gpmi_virtual_chip_can_erase(const IMX6GPMIResetState *s)
{
    return s->virtual_chip.file &&
           s->nand.phase == IMX6_MACRONIX_BLOCK_ERASE_CONFIRM;
}

static bool imx6_gpmi_settings_cow_page(const IMX6GPMIResetState *s,
                                         uint32_t page, uint32_t *index)
{
    if (!s->logical_nand_settings_cow || !s->logical_settings_media.file ||
        page < XZ_SETTINGS_FIRST_PAGE ||
        page - XZ_SETTINGS_FIRST_PAGE >= XZ_SETTINGS_PAGE_COUNT) {
        return false;
    }
    *index = page - XZ_SETTINGS_FIRST_PAGE;
    return true;
}

static bool imx6_gpmi_settings_cow_can_commit(const IMX6GPMIResetState *s)
{
    uint32_t index;

    return s->settings_cow_staged_valid &&
           s->settings_cow_staged_page == s->nand.page_number &&
           s->nand.page_column == 0 &&
           imx6_gpmi_settings_cow_page(s, s->nand.page_number, &index) &&
           s->settings_cow_program_count[index] < 4;
}

static void imx6_gpmi_settings_cow_commit(IMX6GPMIResetState *s)
{
    uint32_t index;
    uint8_t *target;
    size_t byte;

    g_assert(imx6_gpmi_settings_cow_can_commit(s));
    imx6_gpmi_settings_cow_page(s, s->nand.page_number, &index);
    if (!s->settings_cow_pages) {
        s->settings_cow_pages = g_malloc(XZ_SETTINGS_PAGE_COUNT *
                                         IMX6_MACRONIX_PAGE_MAIN_SIZE);
    }
    target = s->settings_cow_pages +
             (size_t)index * IMX6_MACRONIX_PAGE_MAIN_SIZE;
    if (!s->settings_cow_materialized[index]) {
        memcpy(target, s->logical_settings_media.payload_cache +
               (size_t)index * IMX6_MACRONIX_PAGE_MAIN_SIZE,
               IMX6_MACRONIX_PAGE_MAIN_SIZE);
        s->settings_cow_materialized[index] = true;
    }
    /* NAND programming changes cells only from 1 to 0. */
    for (byte = 0; byte < IMX6_MACRONIX_PAGE_MAIN_SIZE; byte++) {
        target[byte] &= s->settings_cow_staged[byte];
    }
    s->settings_cow_program_count[index]++;
    s->settings_cow_staged_valid = false;
    s->nand.last_operation_failed = false;
    /* MX30LF2G18AC ordinary program busy interval, tPROG(max) = 700 us. */
    s->nand.ready = false;
    s->nand.ready_deadline_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 700000;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "XZ_NAND: settings-cow-program page=%u block=%u count=%u busy-ns=700000\n",
                  s->nand.page_number, s->nand.page_number / 64,
                  s->settings_cow_program_count[index]);
}

static bool imx6_gpmi_settings_cow_can_erase(const IMX6GPMIResetState *s)
{
    uint32_t block_first_page;
    uint32_t block_last_page;

    if (s->nand.phase != IMX6_MACRONIX_BLOCK_ERASE_CONFIRM) {
        return false;
    }
    block_first_page = s->nand.page_number & ~UINT32_C(63);
    block_last_page = block_first_page + 63;
    return block_first_page >= XZ_SETTINGS_FIRST_PAGE &&
           block_last_page < XZ_SETTINGS_FIRST_PAGE + XZ_SETTINGS_PAGE_COUNT;
}

static void imx6_gpmi_settings_cow_erase(IMX6GPMIResetState *s)
{
    uint32_t block_first_page = s->nand.page_number & ~UINT32_C(63);
    uint32_t index = block_first_page - XZ_SETTINGS_FIRST_PAGE;
    uint32_t page;

    g_assert(block_first_page >= XZ_SETTINGS_FIRST_PAGE &&
             block_first_page + 63 < XZ_SETTINGS_FIRST_PAGE +
             XZ_SETTINGS_PAGE_COUNT);
    if (!s->settings_cow_pages) {
        s->settings_cow_pages = g_malloc(XZ_SETTINGS_PAGE_COUNT *
                                         IMX6_MACRONIX_PAGE_MAIN_SIZE);
    }
    for (page = 0; page < 64; page++, index++) {
        memset(s->settings_cow_pages +
               (size_t)index * IMX6_MACRONIX_PAGE_MAIN_SIZE, 0xff,
               IMX6_MACRONIX_PAGE_MAIN_SIZE);
        s->settings_cow_materialized[index] = true;
        s->settings_cow_program_count[index] = 0;
    }
    s->settings_cow_staged_valid = false;
    s->nand.last_operation_failed = false;
    /* MX30LF2G18AC block erase bound, tBERS(max) = 10 ms. */
    s->nand.ready = false;
    s->nand.ready_deadline_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 10000000;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "XZ_NAND: settings-cow-erase block=%u first-page=%u busy-ns=10000000\n",
                  block_first_page / 64, block_first_page);
}

bool imx6_gpmi_nand_has_logical_media(const IMX6GPMIResetState *s)
{
    return s->virtual_chip.file || s->logical_media.file || s->logical_kernel_media.file ||
           s->logical_rootfs_media.file || s->logical_pdj_media.file ||
           s->logical_settings_media.file;
}

bool imx6_gpmi_nand_write(IMX6GPMIResetState *s, uint32_t phase,
                          uint32_t chip_select, uint8_t byte)
{
    if (chip_select != 0) {
        return false;
    }
    if (phase == 1) {
        if (byte == 0x10) {
            if (s->virtual_chip.file) {
                if (!imx6_gpmi_virtual_chip_can_commit(s) ||
                    !imx6_macronix_nand_virtual_chip_program_page(
                        &s->nand, &s->virtual_chip, s->virtual_nand_staged,
                        s->virtual_nand_staged_auxiliary0)) {
                    s->nand.last_operation_failed = true;
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "XZ_NAND: virtual-chip-program-rejected page=%u column=%u staged=%u\n",
                                  s->nand.page_number, s->nand.page_column,
                                  s->virtual_nand_staged_valid);
                    return false;
                }
                s->virtual_nand_staged_valid = false;
                return imx6_macronix_nand_command(&s->nand, byte);
            }
            if (!imx6_gpmi_settings_cow_can_commit(s)) {
                s->nand.last_operation_failed = true;
                qemu_log_mask(LOG_GUEST_ERROR,
                              "XZ_NAND: settings-cow-program-rejected page=%u column=%u staged=%u\n",
                              s->nand.page_number, s->nand.page_column,
                              s->settings_cow_staged_valid);
                return false;
            }
            if (!imx6_macronix_nand_command(&s->nand, byte)) {
                return false;
            }
            imx6_gpmi_settings_cow_commit(s);
            return true;
        }
        if (byte == 0xd0) {
            if (s->virtual_chip.file) {
                if (!imx6_gpmi_virtual_chip_can_erase(s) ||
                    !imx6_macronix_nand_virtual_chip_erase_block(
                        &s->nand, &s->virtual_chip)) {
                    s->nand.last_operation_failed = true;
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "XZ_NAND: virtual-chip-erase-rejected page=%u\n",
                                  s->nand.page_number);
                    return false;
                }
                s->virtual_nand_staged_valid = false;
                return imx6_macronix_nand_command(&s->nand, byte);
            }
            if (!imx6_gpmi_settings_cow_can_erase(s)) {
                s->nand.last_operation_failed = true;
                qemu_log_mask(LOG_GUEST_ERROR,
                              "XZ_NAND: settings-cow-erase-rejected page=%u\n",
                              s->nand.page_number);
                return false;
            }
            if (!imx6_macronix_nand_command(&s->nand, byte)) {
                return false;
            }
            imx6_gpmi_settings_cow_erase(s);
            return true;
        }
        bool accepted = imx6_macronix_nand_command(&s->nand, byte);

        if (accepted && byte == 0x30 &&
            s->nand.page_number >= XZ_KERNEL_A_FIRST_PAGE) {
            imx6_gpmi_diagnostic_begin_kernel_phase(s);
        }
        return accepted;
    }
    if (phase == 2) {
        return imx6_macronix_nand_address(&s->nand, byte);
    }
    return false;
}

bool imx6_gpmi_nand_read(IMX6GPMIResetState *s, uint32_t chip_select,
                         uint8_t *bytes, uint32_t length)
{
    uint32_t ctrl0 = s->regs[CTRL0 / sizeof(uint32_t)];
    uint32_t mode = ctrl0 & GPMI_CTRL0_COMMAND_MODE_MASK;
    uint32_t address = (ctrl0 & GPMI_CTRL0_ADDRESS_MASK) >> GPMI_CTRL0_ADDRESS_SHIFT;
    uint32_t cs = (ctrl0 & GPMI_CTRL0_CS_MASK) >> GPMI_CTRL0_CS_SHIFT;

    if (s->nand.page_number >= XZ_KERNEL_A_FIRST_PAGE) {
        imx6_gpmi_diagnostic_begin_kernel_phase(s);
    }
    if (s->diagnostic.enabled) {
        s->diagnostic.nand_transactions++;
    }
    if (chip_select != 0 || cs != 0 || mode != GPMI_CTRL0_COMMAND_MODE_READ ||
        address != GPMI_ADDRESS_DATA) {
        return false;
    }
    /* Preserve the narrow pre-existing U-Boot env-marker transaction. */
    if (s->logical_media.file && s->logical_media.marker_projection &&
        s->nand.phase == IMX6_MACRONIX_PAGE_READ_DATA && length == 1 &&
        s->nand.page_column_cursor == IMX6_MACRONIX_PAGE_MAIN_SIZE &&
        s->nand.page_number >= XZ_ENV_FIRST_PAGE &&
        s->nand.page_number < XZ_ENV_FIRST_PAGE + XZ_ENV_PAGE_COUNT &&
        !(s->nand.page_number - XZ_ENV_FIRST_PAGE) % 64) {
        return imx6_macronix_nand_logical_marker_read(&s->nand,
                                                      &s->logical_media,
                                                      bytes, length);
    }
    /*
     * The separately labelled U-Boot projection gets only the exact one-byte
     * probe.  It must precede the Linux OOB view, whose fail-closed handling
     * otherwise rejects every one-byte OOB-column request.
     */
    if (s->uboot_marker_view.enabled &&
        s->nand.phase == IMX6_MACRONIX_PAGE_READ_DATA &&
        s->nand.page_column_cursor == IMX6_MACRONIX_PAGE_MAIN_SIZE &&
        length == 1) {
        return imx6_macronix_nand_uboot_marker_read(&s->nand,
                                                     &s->uboot_marker_view,
                                                     bytes, length);
    }
    /*
     * This is an opt-in replay of indexed Linux-driver OOB observations.
     * It is never a general raw-OOB or BCH-parity path.  Once an OOB-area
     * request is directed here, malformed or unbacked requests fail closed.
     */
    if (s->logical_oob_view.enabled &&
        s->nand.phase == IMX6_MACRONIX_PAGE_READ_DATA &&
        s->nand.page_column_cursor >= IMX6_MACRONIX_PAGE_MAIN_SIZE) {
        uint32_t settings_index;

        /* A recorded physical OOB sample belongs to the immutable base
         * page.  It must never be presented as the OOB/ECC for a COW page. */
        if (imx6_gpmi_settings_cow_page(s, s->nand.page_number,
                                        &settings_index) &&
            s->settings_cow_materialized[settings_index]) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "XZ_NAND: settings-cow-oob-unavailable page=%u\n",
                          s->nand.page_number);
            return false;
        }
        return imx6_macronix_nand_logical_oob_read(&s->nand,
                                                   &s->logical_oob_view,
                                                   bytes, length);
    }
    /* Logical media has no generic raw/OOB path. */
    if (s->logical_media.file && s->logical_media.marker_projection &&
        s->nand.phase == IMX6_MACRONIX_PAGE_READ_DATA) {
        return imx6_macronix_nand_logical_marker_read(&s->nand,
                                                       &s->logical_media,
                                                       bytes, length);
    }
    if (s->virtual_chip.file) {
        if (s->nand.phase == IMX6_MACRONIX_PAGE_READ_DATA &&
            s->nand.page_column_cursor < IMX6_MACRONIX_PAGE_MAIN_SIZE) {
            return imx6_macronix_nand_virtual_chip_read_main(&s->nand,
                                                              &s->virtual_chip,
                                                              bytes, length);
        }
        if (s->nand.phase == IMX6_MACRONIX_PAGE_READ_DATA &&
            s->nand.page_column_cursor >= IMX6_MACRONIX_PAGE_MAIN_SIZE &&
            length <= IMX6_MACRONIX_PAGE_TOTAL - s->nand.page_column_cursor) {
            /* This is a virtual chip's erased spare plane, required for the
             * ordinary all-good marker scan.  It is never presented to BCH as
             * measured ECC/parity for an imported logical main-area source. */
            memset(bytes, 0xff, length);
            s->nand.page_column_cursor += length;
            s->nand.page_media_status = IMX6_MACRONIX_MEDIA_OK;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "XZ_NAND: virtual-chip-oob-erased page=%u column=%u bytes=%u metadata=virtual\n",
                          s->nand.page_number, s->nand.page_column_cursor - length, length);
            return true;
        }
        if (s->nand.phase == IMX6_MACRONIX_PAGE_READ_DATA) {
            s->nand.page_media_status = IMX6_MACRONIX_MEDIA_LOGICAL_OOB_REJECTED;
            return false;
        }
        /* ID, status and parameter-page streams are device registers, not
         * media.  They retain the common MX30LF2G18AC implementation. */
        return imx6_macronix_nand_read(&s->nand, bytes, length, NULL, false);
    }
    return imx6_macronix_nand_read(&s->nand, bytes, length, &s->media,
                                   s->page_fixture_enabled);
}

bool imx6_gpmi_nand_ready(IMX6GPMIResetState *s, uint32_t chip_select)
{
    return chip_select == 0 && imx6_macronix_nand_ready(&s->nand);
}

int64_t imx6_gpmi_nand_ready_deadline_ns(const IMX6GPMIResetState *s)
{
    return s->nand.ready_deadline_ns;
}

bool imx6_gpmi_nand_page_media_unavailable(const IMX6GPMIResetState *s)
{
    return s->nand.page_media_unavailable;
}

static unsigned imx6_gpmi_hamming_distance(const uint8_t *left,
                                           const uint8_t *right,
                                           size_t length)
{
    unsigned distance = 0;
    size_t index;

    for (index = 0; index < length; index++) {
        distance += ctpop8(left[index] ^ right[index]);
    }
    return distance;
}

static bool imx6_gpmi_all_ff(const uint8_t *bytes, size_t length)
{
    size_t index;

    for (index = 0; index < length; index++) {
        if (bytes[index] != 0xff) {
            return false;
        }
    }
    return true;
}

bool imx6_gpmi_nand_logical_read(IMX6GPMIResetState *s,
                                 uint8_t payload[IMX6_MACRONIX_PAGE_MAIN_SIZE],
                                 uint8_t auxiliary[16], bool *uncorrectable)
{
    uint8_t corrupt[IMX6_MACRONIX_PAGE_MAIN_SIZE];
    unsigned int chunk;

    *uncorrectable = false;
    IMX6MacronixNANDLogicalMedia *region;

    if (s->nand.page_number >= XZ_KERNEL_A_FIRST_PAGE) {
        imx6_gpmi_diagnostic_begin_kernel_phase(s);
    }
    if (s->diagnostic.enabled) {
        s->diagnostic.nand_transactions++;
    }
    if (!imx6_gpmi_nand_has_logical_media(s)) {
        s->nand.page_media_status = IMX6_MACRONIX_MEDIA_UNAVAILABLE;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_NAND: logical-media-unavailable (strict-raw profile)\n");
        return false;
    }
    if (s->virtual_chip.file) {
        if (!imx6_macronix_nand_virtual_chip_read_page(&s->nand,
                                                        &s->virtual_chip,
                                                        payload)) {
            return false;
        }
        /* Main-area image: this is the existing BCH-facing virtual-clean
         * contract, not a physical OOB/ECC assertion. */
        memset(auxiliary, 0xff, 12);
        /* The Pioneer BCH layout consumes this byte as in-band main-area
         * metadata.  Derive it from the delivered page; do not claim OOB. */
        if (!imx6_macronix_nand_virtual_chip_auxiliary0(
                &s->virtual_chip, s->nand.page_number, &auxiliary[0])) {
            auxiliary[0] = payload[1999];
        }
        memset(auxiliary + 12, 0x00, 4);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_NAND: virtual-chip-backed response output-page=%u bytes=%u aux=virtual-clean metadata=virtual\n",
                      s->nand.page_number, IMX6_MACRONIX_PAGE_MAIN_SIZE);
        if (s->nand.cache_final_output) {
            s->nand.phase = IMX6_MACRONIX_IDLE;
            s->nand.cache_valid = false;
            s->nand.cache_pending = false;
            s->nand.cache_stream_active = false;
            s->nand.cache_final_output = false;
        } else if (s->nand.cache_valid &&
                   s->nand.cache_next_page == s->nand.page_number + 1) {
            s->nand.page_number = s->nand.cache_next_page;
            s->nand.page_column = 0;
            s->nand.page_column_cursor = 0;
            s->nand.cache_valid = true;
        }
        region = NULL;
    } else {
        region = imx6_gpmi_logical_media_for_page(s, s->nand.page_number);
        if (!region) {
            s->nand.page_media_status = IMX6_MACRONIX_MEDIA_UNBACKED;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "XZ_NAND: logical-media-unbacked output-page=%u regions=env,kernel_a,rootfs,pdj,settings\n",
                          s->nand.page_number);
            return false;
        }
        if (!imx6_macronix_nand_logical_read(&s->nand, region,
                                             payload, auxiliary)) {
            return false;
        }
    }
    if (region == &s->logical_settings_media && s->logical_nand_settings_cow) {
        uint32_t index;

        if (imx6_gpmi_settings_cow_page(s, s->nand.page_number, &index) &&
            s->settings_cow_materialized[index]) {
            memcpy(payload, s->settings_cow_pages +
                   (size_t)index * IMX6_MACRONIX_PAGE_MAIN_SIZE,
                   IMX6_MACRONIX_PAGE_MAIN_SIZE);
            /* Logical media has no physical OOB/BCH parity.  Keep the
             * established virtual-clean auxiliary surface, never replaying
             * indexed observed OOB bytes after this page was modified. */
            memset(auxiliary, 0xff, 16);
            qemu_log_mask(LOG_GUEST_ERROR,
                          "XZ_NAND: settings-cow-readback page=%u aux=virtual-clean\n",
                          s->nand.page_number);
        }
    }
    if (s->diagnostic.enabled) {
        s->diagnostic.nand_successful_deliveries++;
    }

    /*
     * This is a deterministic test-only correction oracle, not BCH maths or
     * a raw-NAND model.  For chunk N it flips bits N*4096 + [0,count) in a
     * temporary copy, independently measures its Hamming distance from the
     * immutable logical source, then exposes the source payload for <= BCH8.
     */
    memcpy(corrupt, payload, sizeof(corrupt));
    for (chunk = 0; chunk < 4; chunk++) {
        unsigned int requested = (s->logical_nand_test_bitflips >> (chunk * 8)) & 0xff;
        unsigned int bit;
        uint8_t *clean_chunk = payload + chunk * 512;
        uint8_t *corrupt_chunk = corrupt + chunk * 512;
        unsigned int distance;

        for (bit = 0; bit < requested && bit < 512 * 8; bit++) {
            corrupt_chunk[bit / 8] ^= 1U << (bit & 7);
        }
        distance = imx6_gpmi_hamming_distance(clean_chunk, corrupt_chunk, 512);
        if (distance > 8) {
            auxiliary[12 + chunk] = 0xfe;
            *uncorrectable = true;
        } else if (!distance && imx6_gpmi_all_ff(clean_chunk, 512)) {
            /* Known all-ff logical data only; never infer it for an unknown row. */
            auxiliary[12 + chunk] = 0xff;
        } else {
            auxiliary[12 + chunk] = distance;
        }
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "XZ_NAND: logical-test-faults packed=0x%08x uncorrectable=%u\n",
                  s->logical_nand_test_bitflips, *uncorrectable);
    return true;
}

bool imx6_gpmi_nand_stage_program(IMX6GPMIResetState *s,
                                  const uint8_t payload[2048], uint8_t auxiliary0)
{
    uint32_t index;

    if (s->virtual_chip.file) {
        if (!payload || s->nand.phase != IMX6_MACRONIX_PAGE_PROGRAM_DATA ||
            s->nand.page_column != 0 ||
            s->virtual_chip.program_count[s->nand.page_number] >= 4) {
            s->nand.last_operation_failed = true;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "XZ_NAND: virtual-chip-stage-rejected page=%u column=%u\n",
                          s->nand.page_number, s->nand.page_column);
            return false;
        }
        memcpy(s->virtual_nand_staged, payload, IMX6_MACRONIX_PAGE_MAIN_SIZE);
        s->virtual_nand_staged_auxiliary0 = auxiliary0;
        s->virtual_nand_staged_page = s->nand.page_number;
        s->virtual_nand_staged_valid = true;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_NAND: virtual-chip-staged page=%u block=%u\n",
                      s->nand.page_number, s->nand.page_number / 64);
        return true;
    }
    if (!payload || s->nand.phase != IMX6_MACRONIX_PAGE_PROGRAM_DATA ||
        s->nand.page_column != 0 ||
        !imx6_gpmi_settings_cow_page(s, s->nand.page_number, &index) ||
        s->settings_cow_program_count[index] >= 4) {
        s->nand.last_operation_failed = true;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_NAND: settings-cow-stage-rejected page=%u column=%u\n",
                      s->nand.page_number, s->nand.page_column);
        return false;
    }
    memcpy(s->settings_cow_staged, payload, IMX6_MACRONIX_PAGE_MAIN_SIZE);
    s->settings_cow_staged_page = s->nand.page_number;
    s->settings_cow_staged_valid = true;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "XZ_NAND: settings-cow-staged page=%u block=%u\n",
                  s->nand.page_number, s->nand.page_number / 64);
    return true;
}

const char *imx6_gpmi_nand_page_media_stop_reason(const IMX6GPMIResetState *s)
{
    switch (s->nand.page_media_status) {
    case IMX6_MACRONIX_MEDIA_UNAVAILABLE:
        return "nand-page-media-unavailable";
    case IMX6_MACRONIX_MEDIA_UNBACKED:
        return "nand-page-media-unbacked";
    case IMX6_MACRONIX_MEDIA_OUT_OF_RANGE:
        return "nand-page-media-range-invalid";
    case IMX6_MACRONIX_MEDIA_IO_ERROR:
        return "nand-page-media-io-error";
    case IMX6_MACRONIX_MEDIA_COVERAGE_UNKNOWN:
        return "nand-page-media-coverage-unknown";
    case IMX6_MACRONIX_MEDIA_LOGICAL_MARKER_REJECTED:
        return "logical-marker-provenance-rejected";
    case IMX6_MACRONIX_MEDIA_LOGICAL_OOB_REJECTED:
        return "logical-oob-provenance-rejected";
    default:
        return "unsupported-nand-response";
    }
}

static uint64_t imx6_gpmi_reset_read(void *opaque, hwaddr offset,
                                     unsigned size)
{
    IMX6GPMIResetState *s = opaque;

    imx6_gpmi_diagnostic_access(s, IMX6_NAND_DIAG_GPMI, offset, size, false);

    if (offset == GPMI_STAT) {
        uint32_t status = s->regs[offset / sizeof(uint32_t)];

        /*
         * Pioneer U-Boot's GPMI NFC v2 is_ready() reads STAT.READY_BUSY[0].
         * Keep unmodelled status bits shadowable, but derive CS0's live
         * READY/BUSY state from the NAND model on every register read.
         */
        status &= ~GPMI_STAT_READY_BUSY_CS0;
        if (imx6_gpmi_nand_ready(s, 0)) {
            status |= GPMI_STAT_READY_BUSY_CS0;
        }
        return status;
    }

    return s->regs[offset / sizeof(uint32_t)];
}

static void imx6_gpmi_reset_write(void *opaque, hwaddr offset,
                                  uint64_t value, unsigned size)
{
    IMX6GPMIResetState *s = opaque;
    uint32_t bits = value;

    imx6_gpmi_diagnostic_access(s, IMX6_NAND_DIAG_GPMI, offset, size, true);

    switch (offset) {
    case CTRL0:
        s->regs[0] = bits;
        break;
    case CTRL0_SET:
        s->regs[0] |= bits;
        if (bits & MODULE_SFTRST) {
            /* The U-Boot source polls for this hardware side effect. */
            s->regs[0] |= MODULE_CLKGATE;
        }
        break;
    case CTRL0_CLR:
        s->regs[0] &= ~bits;
        break;
    case CTRL0_TOG:
        s->regs[0] ^= bits;
        break;
    default:
        s->regs[offset / sizeof(uint32_t)] = bits;
        break;
    }
}

static const MemoryRegionOps imx6_gpmi_reset_ops = {
    .read = imx6_gpmi_reset_read,
    .write = imx6_gpmi_reset_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = sizeof(uint32_t),
        .max_access_size = sizeof(uint32_t),
    },
};

static void imx6_gpmi_reset_reset(DeviceState *dev)
{
    IMX6GPMIResetState *s = IMX6_GPMI_RESET(dev);

    memset(s->regs, 0, sizeof(s->regs));
    imx6_macronix_nand_reset(&s->nand);
    s->settings_cow_staged_valid = false;
    s->virtual_nand_staged_valid = false;
}

static void imx6_gpmi_reset_init(Object *obj)
{
    IMX6GPMIResetState *s = IMX6_GPMI_RESET(obj);

    memory_region_init_io(&s->iomem, obj, &imx6_gpmi_reset_ops, s,
                          TYPE_IMX6_GPMI_RESET, IMX6_GPMI_RESET_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
}

static void imx6_gpmi_close_logical_regions(IMX6GPMIResetState *s)
{
    imx6_macronix_nand_logical_media_close(&s->logical_settings_media);
    imx6_macronix_nand_logical_media_close(&s->logical_pdj_media);
    imx6_macronix_nand_logical_media_close(&s->logical_rootfs_media);
    imx6_macronix_nand_logical_media_close(&s->logical_kernel_media);
    imx6_macronix_nand_logical_media_close(&s->logical_media);
}

static void imx6_gpmi_reset_realize(DeviceState *dev, Error **errp)
{
    IMX6GPMIResetState *s = IMX6_GPMI_RESET(dev);

    if (s->virtual_nand_path && *s->virtual_nand_path) {
        if (s->nand_media_path || s->nand_media_coverage_path ||
            s->logical_nand_media_path || s->logical_nand_kernel_media_path ||
            s->logical_nand_rootfs_media_path || s->logical_nand_pdj_media_path ||
            s->logical_nand_settings_media_path ||
            s->logical_oob_view_samples_path || s->logical_oob_view_index_path ||
            s->uboot_marker_view_map_path || s->page_fixture_enabled) {
            error_setg(errp, "virtual NAND chip is a complete media profile and cannot be combined with region/raw/OOB fixtures");
            return;
        }
        if (!imx6_macronix_nand_virtual_chip_open(&s->virtual_chip,
                                                   s->virtual_nand_path,
                                                   s->virtual_nand_ram_cache,
                                                   errp)) {
            return;
        }
        s->diagnostic.enabled = s->diagnostic_counters_enabled;
        return;
    }

    if ((s->nand_media_path || s->nand_media_coverage_path ||
         s->logical_nand_media_path || s->logical_nand_kernel_media_path ||
         s->logical_nand_rootfs_media_path || s->logical_nand_pdj_media_path ||
         s->logical_nand_settings_media_path ||
         s->logical_oob_view_samples_path || s->logical_oob_view_index_path ||
         s->uboot_marker_view_map_path) &&
        s->page_fixture_enabled) {
        error_setg(errp, "NAND media and page-fixture cannot both be enabled");
        return;
    }
    if ((s->logical_nand_media_path || s->logical_nand_kernel_media_path ||
         s->logical_nand_rootfs_media_path || s->logical_nand_pdj_media_path ||
         s->logical_nand_settings_media_path ||
         s->logical_oob_view_samples_path || s->logical_oob_view_index_path ||
         s->uboot_marker_view_map_path) &&
        (s->nand_media_path || s->nand_media_coverage_path)) {
        error_setg(errp, "raw nand-media and logical NAND regions are separate profiles");
        return;
    }
    if (s->logical_nand_test_bitflips && !s->logical_nand_media_path &&
        !s->logical_nand_kernel_media_path &&
        !s->logical_nand_rootfs_media_path && !s->logical_nand_pdj_media_path &&
        !s->logical_nand_settings_media_path) {
        error_setg(errp, "logical-nand-test-bitflips requires a logical NAND region");
        return;
    }
    if (s->logical_nand_settings_cow && !s->logical_nand_settings_media_path) {
        error_setg(errp, "logical-nand-settings-cow requires logical-nand-settings-media");
        return;
    }
    if ((s->logical_nand_marker_projection || s->logical_nand_marker_map_path) &&
        !s->logical_nand_media_path) {
        error_setg(errp, "logical marker projection requires logical-nand-media");
        return;
    }
    if (s->uboot_marker_view_enabled != !!(s->uboot_marker_view_map_path &&
                                           *s->uboot_marker_view_map_path)) {
        error_setg(errp, "U-Boot marker projection requires both explicit opt-in and map");
        return;
    }
    if (s->nand_media_coverage_path && !s->nand_media_path) {
        error_setg(errp, "nand-media-coverage requires nand-media");
        return;
    }
    if (!imx6_macronix_nand_media_open(&s->media, s->nand_media_path,
                                       s->nand_media_coverage_path, errp)) {
        return;
    }
    if (!imx6_macronix_nand_logical_media_open(&s->logical_media,
                                                s->logical_nand_media_path,
                                                s->logical_nand_start_page,
                                                s->logical_nand_marker_map_path,
                                                s->logical_nand_marker_projection,
                                                errp)) {
        imx6_macronix_nand_media_close(&s->media);
        return;
    }
    s->diagnostic.enabled = s->diagnostic_counters_enabled;
    if (!imx6_macronix_nand_logical_media_open(&s->logical_kernel_media,
                                                s->logical_nand_kernel_media_path,
                                                s->logical_nand_kernel_start_page,
                                                NULL, false, errp)) {
        imx6_gpmi_close_logical_regions(s);
        imx6_macronix_nand_media_close(&s->media);
        return;
    }
    if (!imx6_macronix_nand_logical_media_open(&s->logical_rootfs_media,
                                                s->logical_nand_rootfs_media_path,
                                                s->logical_nand_rootfs_start_page,
                                                NULL, false, errp)) {
        imx6_gpmi_close_logical_regions(s);
        imx6_macronix_nand_media_close(&s->media);
        return;
    }
    if (!imx6_macronix_nand_logical_media_open(&s->logical_pdj_media,
                                                s->logical_nand_pdj_media_path,
                                                s->logical_nand_pdj_start_page,
                                                NULL, false, errp)) {
        imx6_gpmi_close_logical_regions(s);
        imx6_macronix_nand_media_close(&s->media);
        return;
    }
    if (!imx6_macronix_nand_logical_media_open(&s->logical_settings_media,
                                                s->logical_nand_settings_media_path,
                                                s->logical_nand_settings_start_page,
                                                NULL, false, errp)) {
        imx6_gpmi_close_logical_regions(s);
        imx6_macronix_nand_media_close(&s->media);
        return;
    }
    if (!imx6_macronix_nand_logical_oob_open(&s->logical_oob_view,
                                              s->logical_oob_view_samples_path,
                                              s->logical_oob_view_index_path,
                                              s->logical_oob_view_enabled, errp)) {
        imx6_gpmi_close_logical_regions(s);
        imx6_macronix_nand_media_close(&s->media);
        return;
    }
    if (!imx6_macronix_nand_uboot_marker_open(&s->uboot_marker_view,
                                               s->uboot_marker_view_map_path,
                                               s->uboot_marker_view_enabled, errp)) {
        imx6_macronix_nand_logical_oob_close(&s->logical_oob_view);
        imx6_gpmi_close_logical_regions(s);
        imx6_macronix_nand_media_close(&s->media);
        return;
    }
    if (s->logical_kernel_media.file &&
        (s->logical_kernel_media.start_page != XZ_KERNEL_A_FIRST_PAGE ||
         s->logical_kernel_media.page_count != XZ_KERNEL_A_PAGE_COUNT)) {
        error_setg(errp, "logical kernel-A region must cover pages 2560-6143 exactly");
        imx6_macronix_nand_uboot_marker_close(&s->uboot_marker_view);
        imx6_macronix_nand_logical_oob_close(&s->logical_oob_view);
        imx6_gpmi_close_logical_regions(s);
        imx6_macronix_nand_media_close(&s->media);
        return;
    }
    if (s->logical_media.file &&
        (s->logical_media.start_page != XZ_ENV_FIRST_PAGE ||
         s->logical_media.page_count != XZ_ENV_PAGE_COUNT)) {
        error_setg(errp, "logical env region must cover pages 2048-2559 exactly");
        imx6_macronix_nand_uboot_marker_close(&s->uboot_marker_view);
        imx6_macronix_nand_logical_oob_close(&s->logical_oob_view);
        imx6_gpmi_close_logical_regions(s);
        imx6_macronix_nand_media_close(&s->media);
        return;
    }
    if (s->logical_rootfs_media.file &&
        (s->logical_rootfs_media.start_page != XZ_ROOTFS_FIRST_PAGE ||
         s->logical_rootfs_media.page_count != XZ_ROOTFS_PAGE_COUNT)) {
        error_setg(errp, "logical rootfs region must cover pages 12800-23039 exactly");
        imx6_macronix_nand_uboot_marker_close(&s->uboot_marker_view);
        imx6_macronix_nand_logical_oob_close(&s->logical_oob_view);
        imx6_gpmi_close_logical_regions(s);
        imx6_macronix_nand_media_close(&s->media);
        return;
    }
    if (s->logical_pdj_media.file &&
        (s->logical_pdj_media.start_page != XZ_PDJ_FIRST_PAGE ||
         s->logical_pdj_media.page_count != XZ_PDJ_PAGE_COUNT)) {
        error_setg(errp, "logical pdj region must cover pages 33280-34815 exactly");
        imx6_macronix_nand_uboot_marker_close(&s->uboot_marker_view);
        imx6_macronix_nand_logical_oob_close(&s->logical_oob_view);
        imx6_gpmi_close_logical_regions(s);
        imx6_macronix_nand_media_close(&s->media);
        return;
    }
    if (s->logical_settings_media.file &&
        (s->logical_settings_media.start_page != XZ_SETTINGS_FIRST_PAGE ||
         s->logical_settings_media.page_count != XZ_SETTINGS_PAGE_COUNT)) {
        error_setg(errp, "logical settings region must cover pages 28160-33279 exactly");
        imx6_macronix_nand_uboot_marker_close(&s->uboot_marker_view);
        imx6_macronix_nand_logical_oob_close(&s->logical_oob_view);
        imx6_gpmi_close_logical_regions(s);
        imx6_macronix_nand_media_close(&s->media);
        return;
    }
}

static void imx6_gpmi_reset_unrealize(DeviceState *dev)
{
    IMX6GPMIResetState *s = IMX6_GPMI_RESET(dev);

    imx6_gpmi_diagnostic_dump_phase(s);
    imx6_macronix_nand_virtual_chip_close(&s->virtual_chip);
    imx6_macronix_nand_media_close(&s->media);
    imx6_gpmi_close_logical_regions(s);
    imx6_macronix_nand_logical_oob_close(&s->logical_oob_view);
    imx6_macronix_nand_uboot_marker_close(&s->uboot_marker_view);
    g_free(s->settings_cow_pages);
    s->settings_cow_pages = NULL;
}

static int imx6_gpmi_reset_pre_save(void *opaque)
{
    IMX6GPMIResetState *s = opaque;
    uint32_t index;

    /* The COW overlay is deliberately run-local.  Reject migration rather
     * than silently restoring an immutable base image over guest writes. */
    for (index = 0; index < XZ_SETTINGS_PAGE_COUNT; index++) {
        if (s->settings_cow_materialized[index]) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "XZ_NAND: migration-rejected-dirty-settings-cow\n");
            return -1;
        }
    }

    s->snapshot_virtual_chip_configured = s->virtual_chip.file != NULL;
    s->snapshot_virtual_chip_size = s->virtual_chip.host_size;
    s->snapshot_virtual_chip_device = s->virtual_chip.host_device;
    s->snapshot_virtual_chip_inode = s->virtual_chip.host_inode;
    s->snapshot_virtual_chip_mtime_sec = s->virtual_chip.host_mtime_sec;
    s->snapshot_virtual_chip_mtime_nsec = s->virtual_chip.host_mtime_nsec;

    s->snapshot_media_configured = s->media.file != NULL;
    s->snapshot_media_size = s->media.host_size;
    s->snapshot_media_device = s->media.host_device;
    s->snapshot_media_inode = s->media.host_inode;
    s->snapshot_media_mtime_sec = s->media.host_mtime_sec;
    s->snapshot_media_mtime_nsec = s->media.host_mtime_nsec;
    s->snapshot_coverage_configured = s->media.coverage_file != NULL;
    s->snapshot_coverage_size = s->media.coverage_size;
    s->snapshot_coverage_device = s->media.coverage_device;
    s->snapshot_coverage_inode = s->media.coverage_inode;
    s->snapshot_coverage_mtime_sec = s->media.coverage_mtime_sec;
    s->snapshot_coverage_mtime_nsec = s->media.coverage_mtime_nsec;
    s->snapshot_logical_configured = s->logical_media.file != NULL;
    s->snapshot_logical_size = s->logical_media.host_size;
    s->snapshot_logical_device = s->logical_media.host_device;
    s->snapshot_logical_inode = s->logical_media.host_inode;
    s->snapshot_logical_mtime_sec = s->logical_media.host_mtime_sec;
    s->snapshot_logical_mtime_nsec = s->logical_media.host_mtime_nsec;
    s->snapshot_logical_start_page = s->logical_media.start_page;
    s->snapshot_logical_page_count = s->logical_media.page_count;
    s->snapshot_logical_marker_configured = s->logical_media.marker_map_configured;
    s->snapshot_logical_marker_size = s->logical_media.marker_map_size;
    s->snapshot_logical_marker_device = s->logical_media.marker_map_device;
    s->snapshot_logical_marker_inode = s->logical_media.marker_map_inode;
    s->snapshot_logical_marker_mtime_sec = s->logical_media.marker_map_mtime_sec;
    s->snapshot_logical_marker_mtime_nsec = s->logical_media.marker_map_mtime_nsec;
    s->snapshot_logical_kernel_configured = s->logical_kernel_media.file != NULL;
    s->snapshot_logical_kernel_size = s->logical_kernel_media.host_size;
    s->snapshot_logical_kernel_device = s->logical_kernel_media.host_device;
    s->snapshot_logical_kernel_inode = s->logical_kernel_media.host_inode;
    s->snapshot_logical_kernel_mtime_sec = s->logical_kernel_media.host_mtime_sec;
    s->snapshot_logical_kernel_mtime_nsec = s->logical_kernel_media.host_mtime_nsec;
    s->snapshot_logical_kernel_start_page = s->logical_kernel_media.start_page;
    s->snapshot_logical_kernel_page_count = s->logical_kernel_media.page_count;
    s->snapshot_logical_rootfs_configured = s->logical_rootfs_media.file != NULL;
    s->snapshot_logical_rootfs_size = s->logical_rootfs_media.host_size;
    s->snapshot_logical_rootfs_device = s->logical_rootfs_media.host_device;
    s->snapshot_logical_rootfs_inode = s->logical_rootfs_media.host_inode;
    s->snapshot_logical_rootfs_mtime_sec = s->logical_rootfs_media.host_mtime_sec;
    s->snapshot_logical_rootfs_mtime_nsec = s->logical_rootfs_media.host_mtime_nsec;
    s->snapshot_logical_rootfs_start_page = s->logical_rootfs_media.start_page;
    s->snapshot_logical_rootfs_page_count = s->logical_rootfs_media.page_count;
    s->snapshot_logical_pdj_configured = s->logical_pdj_media.file != NULL;
    s->snapshot_logical_pdj_size = s->logical_pdj_media.host_size;
    s->snapshot_logical_pdj_device = s->logical_pdj_media.host_device;
    s->snapshot_logical_pdj_inode = s->logical_pdj_media.host_inode;
    s->snapshot_logical_pdj_mtime_sec = s->logical_pdj_media.host_mtime_sec;
    s->snapshot_logical_pdj_mtime_nsec = s->logical_pdj_media.host_mtime_nsec;
    s->snapshot_logical_pdj_start_page = s->logical_pdj_media.start_page;
    s->snapshot_logical_pdj_page_count = s->logical_pdj_media.page_count;
    s->snapshot_logical_settings_configured = s->logical_settings_media.file != NULL;
    s->snapshot_logical_settings_size = s->logical_settings_media.host_size;
    s->snapshot_logical_settings_device = s->logical_settings_media.host_device;
    s->snapshot_logical_settings_inode = s->logical_settings_media.host_inode;
    s->snapshot_logical_settings_mtime_sec = s->logical_settings_media.host_mtime_sec;
    s->snapshot_logical_settings_mtime_nsec = s->logical_settings_media.host_mtime_nsec;
    s->snapshot_logical_settings_start_page = s->logical_settings_media.start_page;
    s->snapshot_logical_settings_page_count = s->logical_settings_media.page_count;
    s->snapshot_logical_oob_configured = s->logical_oob_view.enabled;
    s->snapshot_logical_oob_samples_size = s->logical_oob_view.samples_size;
    s->snapshot_logical_oob_samples_device = s->logical_oob_view.samples_device;
    s->snapshot_logical_oob_samples_inode = s->logical_oob_view.samples_inode;
    s->snapshot_logical_oob_samples_mtime_sec = s->logical_oob_view.samples_mtime_sec;
    s->snapshot_logical_oob_samples_mtime_nsec = s->logical_oob_view.samples_mtime_nsec;
    s->snapshot_logical_oob_index_size = s->logical_oob_view.index_size;
    s->snapshot_logical_oob_index_device = s->logical_oob_view.index_device;
    s->snapshot_logical_oob_index_inode = s->logical_oob_view.index_inode;
    s->snapshot_logical_oob_index_mtime_sec = s->logical_oob_view.index_mtime_sec;
    s->snapshot_logical_oob_index_mtime_nsec = s->logical_oob_view.index_mtime_nsec;
    s->snapshot_uboot_marker_configured = s->uboot_marker_view.enabled;
    s->snapshot_uboot_marker_size = s->uboot_marker_view.map_size;
    s->snapshot_uboot_marker_device = s->uboot_marker_view.map_device;
    s->snapshot_uboot_marker_inode = s->uboot_marker_view.map_inode;
    s->snapshot_uboot_marker_mtime_sec = s->uboot_marker_view.map_mtime_sec;
    s->snapshot_uboot_marker_mtime_nsec = s->uboot_marker_view.map_mtime_nsec;
    return 0;
}

static int imx6_gpmi_reset_post_load(void *opaque, int version_id)
{
    IMX6GPMIResetState *s = opaque;

    /* P13-S adds persistent status state.  It did not exist in older saves. */
    if (version_id < 17) {
        s->nand.write_protected = false;
        s->nand.last_operation_failed = false;
    }
    if (version_id < 5) {
        return 0;
    }
    if (version_id >= 18 && s->snapshot_virtual_chip_configured &&
        !imx6_macronix_nand_virtual_chip_matches(
            &s->virtual_chip, s->snapshot_virtual_chip_size,
            s->snapshot_virtual_chip_device, s->snapshot_virtual_chip_inode,
            s->snapshot_virtual_chip_mtime_sec,
            s->snapshot_virtual_chip_mtime_nsec)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_NAND: snapshot-virtual-chip-identity-mismatch\n");
        return -1;
    }
    if (s->snapshot_media_configured &&
        !imx6_macronix_nand_media_matches(&s->media,
                                           s->snapshot_media_size,
                                           s->snapshot_media_device,
                                           s->snapshot_media_inode,
                                           s->snapshot_media_mtime_sec,
                                           s->snapshot_media_mtime_nsec)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_NAND: snapshot-media-identity-mismatch\n");
        return -1;
    }
    if (version_id >= 6 && s->snapshot_coverage_configured &&
        !imx6_macronix_nand_media_coverage_matches(&s->media,
                                                    s->snapshot_coverage_size,
                                                    s->snapshot_coverage_device,
                                                    s->snapshot_coverage_inode,
                                                    s->snapshot_coverage_mtime_sec,
                                                    s->snapshot_coverage_mtime_nsec)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_NAND: snapshot-media-coverage-identity-mismatch\n");
        return -1;
    }
    if (version_id >= 8 && s->snapshot_logical_configured &&
        !imx6_macronix_nand_logical_media_matches(
            &s->logical_media, s->snapshot_logical_size,
            s->snapshot_logical_device, s->snapshot_logical_inode,
            s->snapshot_logical_mtime_sec, s->snapshot_logical_mtime_nsec,
            s->snapshot_logical_start_page, s->snapshot_logical_page_count)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_NAND: snapshot-logical-media-identity-mismatch\n");
        return -1;
    }
    if (version_id >= 11 && s->snapshot_logical_marker_configured &&
        !imx6_macronix_nand_logical_marker_map_matches(
            &s->logical_media, s->snapshot_logical_marker_size,
            s->snapshot_logical_marker_device, s->snapshot_logical_marker_inode,
            s->snapshot_logical_marker_mtime_sec,
            s->snapshot_logical_marker_mtime_nsec)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_NAND: snapshot-logical-marker-map-identity-mismatch\n");
        return -1;
    }
    if (version_id >= 12 && s->snapshot_logical_kernel_configured &&
        !imx6_macronix_nand_logical_media_matches(
            &s->logical_kernel_media, s->snapshot_logical_kernel_size,
            s->snapshot_logical_kernel_device, s->snapshot_logical_kernel_inode,
            s->snapshot_logical_kernel_mtime_sec,
            s->snapshot_logical_kernel_mtime_nsec,
            s->snapshot_logical_kernel_start_page,
            s->snapshot_logical_kernel_page_count)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_NAND: snapshot-logical-kernel-media-identity-mismatch\n");
        return -1;
    }
    if (version_id >= 15 && s->snapshot_logical_rootfs_configured &&
        !imx6_macronix_nand_logical_media_matches(
            &s->logical_rootfs_media, s->snapshot_logical_rootfs_size,
            s->snapshot_logical_rootfs_device, s->snapshot_logical_rootfs_inode,
            s->snapshot_logical_rootfs_mtime_sec,
            s->snapshot_logical_rootfs_mtime_nsec,
            s->snapshot_logical_rootfs_start_page,
            s->snapshot_logical_rootfs_page_count)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_NAND: snapshot-logical-rootfs-media-identity-mismatch\n");
        return -1;
    }
    if (version_id >= 15 && s->snapshot_logical_pdj_configured &&
        !imx6_macronix_nand_logical_media_matches(
            &s->logical_pdj_media, s->snapshot_logical_pdj_size,
            s->snapshot_logical_pdj_device, s->snapshot_logical_pdj_inode,
            s->snapshot_logical_pdj_mtime_sec,
            s->snapshot_logical_pdj_mtime_nsec,
            s->snapshot_logical_pdj_start_page,
            s->snapshot_logical_pdj_page_count)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_NAND: snapshot-logical-pdj-media-identity-mismatch\n");
        return -1;
    }
    if (version_id >= 16 && s->snapshot_logical_settings_configured &&
        !imx6_macronix_nand_logical_media_matches(
            &s->logical_settings_media, s->snapshot_logical_settings_size,
            s->snapshot_logical_settings_device, s->snapshot_logical_settings_inode,
            s->snapshot_logical_settings_mtime_sec,
            s->snapshot_logical_settings_mtime_nsec,
            s->snapshot_logical_settings_start_page,
            s->snapshot_logical_settings_page_count)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_NAND: snapshot-logical-settings-media-identity-mismatch\n");
        return -1;
    }
    if (version_id >= 13 && s->snapshot_logical_oob_configured &&
        !imx6_macronix_nand_logical_oob_matches(
            &s->logical_oob_view, s->snapshot_logical_oob_samples_size,
            s->snapshot_logical_oob_samples_device,
            s->snapshot_logical_oob_samples_inode,
            s->snapshot_logical_oob_samples_mtime_sec,
            s->snapshot_logical_oob_samples_mtime_nsec,
            s->snapshot_logical_oob_index_size,
            s->snapshot_logical_oob_index_device,
            s->snapshot_logical_oob_index_inode,
            s->snapshot_logical_oob_index_mtime_sec,
            s->snapshot_logical_oob_index_mtime_nsec)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_NAND: snapshot-logical-oob-view-identity-mismatch\n");
        return -1;
    }
    if (version_id >= 14 && s->snapshot_uboot_marker_configured &&
        !imx6_macronix_nand_uboot_marker_matches(
            &s->uboot_marker_view, s->snapshot_uboot_marker_size,
            s->snapshot_uboot_marker_device, s->snapshot_uboot_marker_inode,
            s->snapshot_uboot_marker_mtime_sec,
            s->snapshot_uboot_marker_mtime_nsec)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_NAND: snapshot-uboot-marker-map-identity-mismatch\n");
        return -1;
    }
    return 0;
}

static const VMStateDescription vmstate_imx6_gpmi_reset = {
    .name = TYPE_IMX6_GPMI_RESET,
    .version_id = 18,
    .minimum_version_id = 1,
    .pre_save = imx6_gpmi_reset_pre_save,
    .post_load = imx6_gpmi_reset_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX6GPMIResetState,
                             IMX6_GPMI_RESET_SIZE / sizeof(uint32_t)),
        VMSTATE_UINT32_V(nand.phase, IMX6GPMIResetState, 2),
        VMSTATE_UINT8_ARRAY_V(nand.response, IMX6GPMIResetState, 5, 2),
        VMSTATE_UINT8_V(nand.response_length, IMX6GPMIResetState, 2),
        VMSTATE_UINT8_V(nand.response_offset, IMX6GPMIResetState, 2),
        VMSTATE_BOOL_V(nand.ready, IMX6GPMIResetState, 2),
        VMSTATE_UINT16_V(nand.parameter_page_offset, IMX6GPMIResetState, 3),
        VMSTATE_UINT8_ARRAY_V(nand.page_address, IMX6GPMIResetState, 5, 4),
        VMSTATE_UINT8_V(nand.page_address_count, IMX6GPMIResetState, 4),
        VMSTATE_UINT16_V(nand.page_column, IMX6GPMIResetState, 4),
        VMSTATE_UINT16_V(nand.page_column_cursor, IMX6GPMIResetState, 4),
        VMSTATE_UINT32_V(nand.page_number, IMX6GPMIResetState, 4),
        VMSTATE_BOOL_V(nand.page_media_unavailable, IMX6GPMIResetState, 4),
        VMSTATE_UINT8_V(nand.page_media_status, IMX6GPMIResetState, 5),
        VMSTATE_BOOL_V(snapshot_media_configured, IMX6GPMIResetState, 5),
        VMSTATE_UINT64_V(snapshot_media_size, IMX6GPMIResetState, 5),
        VMSTATE_UINT64_V(snapshot_media_device, IMX6GPMIResetState, 5),
        VMSTATE_UINT64_V(snapshot_media_inode, IMX6GPMIResetState, 5),
        VMSTATE_INT64_V(snapshot_media_mtime_sec, IMX6GPMIResetState, 5),
        VMSTATE_INT64_V(snapshot_media_mtime_nsec, IMX6GPMIResetState, 5),
        VMSTATE_BOOL_V(snapshot_coverage_configured, IMX6GPMIResetState, 6),
        VMSTATE_UINT64_V(snapshot_coverage_size, IMX6GPMIResetState, 6),
        VMSTATE_UINT64_V(snapshot_coverage_device, IMX6GPMIResetState, 6),
        VMSTATE_UINT64_V(snapshot_coverage_inode, IMX6GPMIResetState, 6),
        VMSTATE_INT64_V(snapshot_coverage_mtime_sec, IMX6GPMIResetState, 6),
        VMSTATE_INT64_V(snapshot_coverage_mtime_nsec, IMX6GPMIResetState, 6),
        VMSTATE_UINT8_V(nand.cache_address_count, IMX6GPMIResetState, 7),
        VMSTATE_UINT32_V(nand.cache_next_page, IMX6GPMIResetState, 7),
        VMSTATE_BOOL_V(nand.cache_valid, IMX6GPMIResetState, 7),
        VMSTATE_BOOL_V(nand.cache_pending, IMX6GPMIResetState, 7),
        VMSTATE_INT64_V(nand.ready_deadline_ns, IMX6GPMIResetState, 7),
        VMSTATE_BOOL_V(snapshot_logical_configured, IMX6GPMIResetState, 8),
        VMSTATE_UINT64_V(snapshot_logical_size, IMX6GPMIResetState, 8),
        VMSTATE_UINT64_V(snapshot_logical_device, IMX6GPMIResetState, 8),
        VMSTATE_UINT64_V(snapshot_logical_inode, IMX6GPMIResetState, 8),
        VMSTATE_INT64_V(snapshot_logical_mtime_sec, IMX6GPMIResetState, 8),
        VMSTATE_INT64_V(snapshot_logical_mtime_nsec, IMX6GPMIResetState, 8),
        VMSTATE_UINT32_V(snapshot_logical_start_page, IMX6GPMIResetState, 8),
        VMSTATE_UINT32_V(snapshot_logical_page_count, IMX6GPMIResetState, 8),
        VMSTATE_UINT32_V(logical_nand_test_bitflips, IMX6GPMIResetState, 9),
        VMSTATE_BOOL_V(nand.cache_stream_active, IMX6GPMIResetState, 10),
        VMSTATE_BOOL_V(nand.cache_final_output, IMX6GPMIResetState, 10),
        VMSTATE_BOOL_V(snapshot_logical_marker_configured, IMX6GPMIResetState, 11),
        VMSTATE_UINT64_V(snapshot_logical_marker_size, IMX6GPMIResetState, 11),
        VMSTATE_UINT64_V(snapshot_logical_marker_device, IMX6GPMIResetState, 11),
        VMSTATE_UINT64_V(snapshot_logical_marker_inode, IMX6GPMIResetState, 11),
        VMSTATE_INT64_V(snapshot_logical_marker_mtime_sec, IMX6GPMIResetState, 11),
        VMSTATE_INT64_V(snapshot_logical_marker_mtime_nsec, IMX6GPMIResetState, 11),
        VMSTATE_BOOL_V(snapshot_logical_kernel_configured, IMX6GPMIResetState, 12),
        VMSTATE_UINT64_V(snapshot_logical_kernel_size, IMX6GPMIResetState, 12),
        VMSTATE_UINT64_V(snapshot_logical_kernel_device, IMX6GPMIResetState, 12),
        VMSTATE_UINT64_V(snapshot_logical_kernel_inode, IMX6GPMIResetState, 12),
        VMSTATE_INT64_V(snapshot_logical_kernel_mtime_sec, IMX6GPMIResetState, 12),
        VMSTATE_INT64_V(snapshot_logical_kernel_mtime_nsec, IMX6GPMIResetState, 12),
        VMSTATE_UINT32_V(snapshot_logical_kernel_start_page, IMX6GPMIResetState, 12),
        VMSTATE_UINT32_V(snapshot_logical_kernel_page_count, IMX6GPMIResetState, 12),
        VMSTATE_BOOL_V(snapshot_logical_rootfs_configured, IMX6GPMIResetState, 15),
        VMSTATE_UINT64_V(snapshot_logical_rootfs_size, IMX6GPMIResetState, 15),
        VMSTATE_UINT64_V(snapshot_logical_rootfs_device, IMX6GPMIResetState, 15),
        VMSTATE_UINT64_V(snapshot_logical_rootfs_inode, IMX6GPMIResetState, 15),
        VMSTATE_INT64_V(snapshot_logical_rootfs_mtime_sec, IMX6GPMIResetState, 15),
        VMSTATE_INT64_V(snapshot_logical_rootfs_mtime_nsec, IMX6GPMIResetState, 15),
        VMSTATE_UINT32_V(snapshot_logical_rootfs_start_page, IMX6GPMIResetState, 15),
        VMSTATE_UINT32_V(snapshot_logical_rootfs_page_count, IMX6GPMIResetState, 15),
        VMSTATE_BOOL_V(snapshot_logical_pdj_configured, IMX6GPMIResetState, 15),
        VMSTATE_UINT64_V(snapshot_logical_pdj_size, IMX6GPMIResetState, 15),
        VMSTATE_UINT64_V(snapshot_logical_pdj_device, IMX6GPMIResetState, 15),
        VMSTATE_UINT64_V(snapshot_logical_pdj_inode, IMX6GPMIResetState, 15),
        VMSTATE_INT64_V(snapshot_logical_pdj_mtime_sec, IMX6GPMIResetState, 15),
        VMSTATE_INT64_V(snapshot_logical_pdj_mtime_nsec, IMX6GPMIResetState, 15),
        VMSTATE_UINT32_V(snapshot_logical_pdj_start_page, IMX6GPMIResetState, 15),
        VMSTATE_UINT32_V(snapshot_logical_pdj_page_count, IMX6GPMIResetState, 15),
        VMSTATE_BOOL_V(snapshot_logical_settings_configured, IMX6GPMIResetState, 16),
        VMSTATE_UINT64_V(snapshot_logical_settings_size, IMX6GPMIResetState, 16),
        VMSTATE_UINT64_V(snapshot_logical_settings_device, IMX6GPMIResetState, 16),
        VMSTATE_UINT64_V(snapshot_logical_settings_inode, IMX6GPMIResetState, 16),
        VMSTATE_INT64_V(snapshot_logical_settings_mtime_sec, IMX6GPMIResetState, 16),
        VMSTATE_INT64_V(snapshot_logical_settings_mtime_nsec, IMX6GPMIResetState, 16),
        VMSTATE_UINT32_V(snapshot_logical_settings_start_page, IMX6GPMIResetState, 16),
        VMSTATE_UINT32_V(snapshot_logical_settings_page_count, IMX6GPMIResetState, 16),
        VMSTATE_BOOL_V(nand.write_protected, IMX6GPMIResetState, 17),
        VMSTATE_BOOL_V(nand.last_operation_failed, IMX6GPMIResetState, 17),
        VMSTATE_BOOL_V(snapshot_virtual_chip_configured, IMX6GPMIResetState, 18),
        VMSTATE_UINT64_V(snapshot_virtual_chip_size, IMX6GPMIResetState, 18),
        VMSTATE_UINT64_V(snapshot_virtual_chip_device, IMX6GPMIResetState, 18),
        VMSTATE_UINT64_V(snapshot_virtual_chip_inode, IMX6GPMIResetState, 18),
        VMSTATE_INT64_V(snapshot_virtual_chip_mtime_sec, IMX6GPMIResetState, 18),
        VMSTATE_INT64_V(snapshot_virtual_chip_mtime_nsec, IMX6GPMIResetState, 18),
        VMSTATE_BOOL_V(snapshot_logical_oob_configured, IMX6GPMIResetState, 13),
        VMSTATE_UINT64_V(snapshot_logical_oob_samples_size, IMX6GPMIResetState, 13),
        VMSTATE_UINT64_V(snapshot_logical_oob_samples_device, IMX6GPMIResetState, 13),
        VMSTATE_UINT64_V(snapshot_logical_oob_samples_inode, IMX6GPMIResetState, 13),
        VMSTATE_INT64_V(snapshot_logical_oob_samples_mtime_sec, IMX6GPMIResetState, 13),
        VMSTATE_INT64_V(snapshot_logical_oob_samples_mtime_nsec, IMX6GPMIResetState, 13),
        VMSTATE_UINT64_V(snapshot_logical_oob_index_size, IMX6GPMIResetState, 13),
        VMSTATE_UINT64_V(snapshot_logical_oob_index_device, IMX6GPMIResetState, 13),
        VMSTATE_UINT64_V(snapshot_logical_oob_index_inode, IMX6GPMIResetState, 13),
        VMSTATE_INT64_V(snapshot_logical_oob_index_mtime_sec, IMX6GPMIResetState, 13),
        VMSTATE_INT64_V(snapshot_logical_oob_index_mtime_nsec, IMX6GPMIResetState, 13),
        VMSTATE_BOOL_V(snapshot_uboot_marker_configured, IMX6GPMIResetState, 14),
        VMSTATE_UINT64_V(snapshot_uboot_marker_size, IMX6GPMIResetState, 14),
        VMSTATE_UINT64_V(snapshot_uboot_marker_device, IMX6GPMIResetState, 14),
        VMSTATE_UINT64_V(snapshot_uboot_marker_inode, IMX6GPMIResetState, 14),
        VMSTATE_INT64_V(snapshot_uboot_marker_mtime_sec, IMX6GPMIResetState, 14),
        VMSTATE_INT64_V(snapshot_uboot_marker_mtime_nsec, IMX6GPMIResetState, 14),
        VMSTATE_END_OF_LIST()
    },
};

static const Property imx6_gpmi_reset_properties[] = {
    DEFINE_PROP_STRING("nand-media", IMX6GPMIResetState, nand_media_path),
    DEFINE_PROP_STRING("nand-media-coverage", IMX6GPMIResetState,
                       nand_media_coverage_path),
    DEFINE_PROP_STRING("virtual-nand", IMX6GPMIResetState, virtual_nand_path),
    DEFINE_PROP_STRING("logical-nand-media", IMX6GPMIResetState,
                       logical_nand_media_path),
    DEFINE_PROP_STRING("logical-nand-kernel-media", IMX6GPMIResetState,
                       logical_nand_kernel_media_path),
    DEFINE_PROP_STRING("logical-nand-rootfs-media", IMX6GPMIResetState,
                       logical_nand_rootfs_media_path),
    DEFINE_PROP_STRING("logical-nand-pdj-media", IMX6GPMIResetState,
                       logical_nand_pdj_media_path),
    DEFINE_PROP_STRING("logical-nand-settings-media", IMX6GPMIResetState,
                       logical_nand_settings_media_path),
    DEFINE_PROP_STRING("logical-nand-marker-map", IMX6GPMIResetState,
                       logical_nand_marker_map_path),
    DEFINE_PROP_STRING("logical-oob-view-samples", IMX6GPMIResetState,
                       logical_oob_view_samples_path),
    DEFINE_PROP_STRING("logical-oob-view-index", IMX6GPMIResetState,
                       logical_oob_view_index_path),
    DEFINE_PROP_STRING("uboot-marker-view-map", IMX6GPMIResetState,
                       uboot_marker_view_map_path),
    DEFINE_PROP_BOOL("logical-nand-marker-projection", IMX6GPMIResetState,
                     logical_nand_marker_projection, false),
    DEFINE_PROP_BOOL("logical-oob-view-enable", IMX6GPMIResetState,
                     logical_oob_view_enabled, false),
    DEFINE_PROP_BOOL("uboot-marker-view-enable", IMX6GPMIResetState,
                     uboot_marker_view_enabled, false),
    DEFINE_PROP_BOOL("logical-nand-settings-cow", IMX6GPMIResetState,
                     logical_nand_settings_cow, false),
    DEFINE_PROP_UINT32("logical-nand-start-page", IMX6GPMIResetState,
                       logical_nand_start_page, 2048),
    DEFINE_PROP_UINT32("logical-nand-kernel-start-page", IMX6GPMIResetState,
                       logical_nand_kernel_start_page, 2560),
    DEFINE_PROP_UINT32("logical-nand-rootfs-start-page", IMX6GPMIResetState,
                       logical_nand_rootfs_start_page, 12800),
    DEFINE_PROP_UINT32("logical-nand-pdj-start-page", IMX6GPMIResetState,
                       logical_nand_pdj_start_page, 33280),
    DEFINE_PROP_UINT32("logical-nand-settings-start-page", IMX6GPMIResetState,
                       logical_nand_settings_start_page, 28160),
    DEFINE_PROP_UINT32("logical-nand-test-bitflips", IMX6GPMIResetState,
                       logical_nand_test_bitflips, 0),
    DEFINE_PROP_BOOL("page-fixture", IMX6GPMIResetState,
                     page_fixture_enabled, false),
    DEFINE_PROP_BOOL("virtual-nand-ram-cache", IMX6GPMIResetState,
                     virtual_nand_ram_cache, false),
    DEFINE_PROP_BOOL("diagnostic-counters", IMX6GPMIResetState,
                     diagnostic_counters_enabled, false),
};

static void imx6_gpmi_reset_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, imx6_gpmi_reset_reset);
    dc->realize = imx6_gpmi_reset_realize;
    dc->unrealize = imx6_gpmi_reset_unrealize;
    device_class_set_props(dc, imx6_gpmi_reset_properties);
    dc->vmsd = &vmstate_imx6_gpmi_reset;
    dc->desc = "i.MX6 GPMI-family reset register block";
}

static const TypeInfo imx6_gpmi_reset_type_info = {
    .name = TYPE_IMX6_GPMI_RESET,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMX6GPMIResetState),
    .instance_init = imx6_gpmi_reset_init,
    .class_init = imx6_gpmi_reset_class_init,
};

static void imx6_gpmi_reset_register_types(void)
{
    type_register_static(&imx6_gpmi_reset_type_info);
}

type_init(imx6_gpmi_reset_register_types)
