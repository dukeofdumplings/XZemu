/* Minimal Macronix MX30LF2G18AC command/response model for XDJ-XZ. */
#ifndef HW_MISC_IMX6_MACRONIX_NAND_H
#define HW_MISC_IMX6_MACRONIX_NAND_H

#include "qemu/osdep.h"
#include "qapi/error.h"

typedef enum IMX6MacronixPhase {
    IMX6_MACRONIX_IDLE,
    IMX6_MACRONIX_READ_ID_ADDRESS,
    IMX6_MACRONIX_READ_ID_DATA,
    IMX6_MACRONIX_PARAM_PAGE_ADDRESS,
    IMX6_MACRONIX_PARAM_PAGE_DATA,
    IMX6_MACRONIX_PAGE_READ_ADDRESS,
    IMX6_MACRONIX_PAGE_READ_PENDING,
    IMX6_MACRONIX_PAGE_READ_DATA,
    IMX6_MACRONIX_CACHE_SEQUENTIAL_ADDRESS,
    /* XDJ-XZ helper form: 3f followed by exactly two zero column bytes. */
    IMX6_MACRONIX_CACHE_FINAL_ADDRESS,
    /* NAND_CMD_STATUS (0x70): persists until a subsequent command.
     * Keep this last: phase values are migration state. */
    IMX6_MACRONIX_STATUS_DATA,
    IMX6_MACRONIX_PAGE_PROGRAM_ADDRESS,
    IMX6_MACRONIX_PAGE_PROGRAM_DATA,
    IMX6_MACRONIX_BLOCK_ERASE_ADDRESS,
    IMX6_MACRONIX_BLOCK_ERASE_CONFIRM,
} IMX6MacronixPhase;

#define IMX6_MACRONIX_PARAM_PAGE_SIZE 256
#define IMX6_MACRONIX_PARAM_PAGE_COPIES 3
#define IMX6_MACRONIX_PARAM_PAGE_TOTAL \
    (IMX6_MACRONIX_PARAM_PAGE_SIZE * IMX6_MACRONIX_PARAM_PAGE_COPIES)
#define IMX6_MACRONIX_PAGE_MAIN_SIZE 2048
#define IMX6_MACRONIX_PAGE_OOB_SIZE 64
#define IMX6_MACRONIX_PAGE_TOTAL \
    (IMX6_MACRONIX_PAGE_MAIN_SIZE + IMX6_MACRONIX_PAGE_OOB_SIZE)
#define IMX6_MACRONIX_PAGE_COUNT (64 * 2048)
#define IMX6_MACRONIX_MEDIA_SIZE \
    ((uint64_t)IMX6_MACRONIX_PAGE_TOTAL * IMX6_MACRONIX_PAGE_COUNT)

/*
 * A raw physical-page image is a sequence of 2048-byte main areas followed
 * by their 64-byte OOB areas.  The backend deliberately owns no command
 * state: it is only an immutable byte source for an already decoded read.
 */
typedef struct IMX6MacronixNANDMedia {
    FILE *file;
    FILE *coverage_file;
    uint64_t host_size;
    uint64_t host_device;
    uint64_t host_inode;
    int64_t host_mtime_sec;
    int64_t host_mtime_nsec;
    uint64_t coverage_size;
    uint64_t coverage_device;
    uint64_t coverage_inode;
    int64_t coverage_mtime_sec;
    int64_t coverage_mtime_nsec;
    uint32_t host_pages;
} IMX6MacronixNANDMedia;

/* Corrected logical MTD bytes; this intentionally contains no raw OOB/ECC. */
typedef struct IMX6MacronixNANDLogicalMedia {
    FILE *file;
    /*
     * Immutable logical media is validated when it is attached.  Keeping its
     * exact bytes resident avoids a host seek/read for every 2048-byte NAND
     * response while retaining the file handle for migration identity checks.
     */
    uint8_t *payload_cache;
    uint64_t host_size;
    uint64_t host_device;
    uint64_t host_inode;
    int64_t host_mtime_sec;
    int64_t host_mtime_nsec;
    uint32_t start_page;
    uint32_t page_count;
    /* Explicitly opt-in virtual projection of the captured env block map. */
    bool marker_projection;
    /* Marker map identity is distinct from logical payload identity. */
    bool marker_map_configured;
    uint64_t marker_map_size;
    uint64_t marker_map_device;
    uint64_t marker_map_inode;
    int64_t marker_map_mtime_sec;
    int64_t marker_map_mtime_nsec;
    uint8_t block_health[8]; /* 0=GOOD, 1=BAD; never inferred outside env. */
} IMX6MacronixNANDLogicalMedia;

/*
 * P14 virtual chip backing.  The file contains exactly one 2048-byte main
 * area for every MX30LF2G18AC page (256 MiB).  It intentionally has no OOB
 * plane: a logical-main seed has no observed spare/ECC bytes to preserve.
 * The controller supplies its existing virtual-clean BCH result for main
 * area transfers and rejects physical-OOB claims elsewhere.
 */
typedef struct IMX6MacronixNANDVirtualChip {
    FILE *file;
    /*
     * Guest BCH writes carry one source-observed byte outside the 2048-byte
     * main area.  ``*.bchaux`` is an optional, private companion of this
     * main-area image; it is not a physical OOB/ECC capture.
     */
    FILE *aux_file;
    char *aux_path;
    uint8_t *aux_valid;
    uint8_t *aux_value;
    uint8_t *ram_cache;
    uint8_t *program_count;
    uint64_t host_size;
    uint64_t host_device;
    uint64_t host_inode;
    int64_t host_mtime_sec;
    int64_t host_mtime_nsec;
    bool ram_cache_enabled;
} IMX6MacronixNANDVirtualChip;

/*
 * Explicit diagnostic replay of the run-0005 Linux MTD OOB view.  This is
 * intentionally a separate source from corrected logical main-area media:
 * it carries only indexed 64-byte driver-returned OOB observations.
 */
#define IMX6_MACRONIX_LOGICAL_OOB_SAMPLE_COUNT 4096
typedef struct IMX6MacronixNANDLogicalOOBView {
    FILE *samples_file;
    uint64_t samples_size;
    uint64_t samples_device;
    uint64_t samples_inode;
    int64_t samples_mtime_sec;
    int64_t samples_mtime_nsec;
    uint64_t index_size;
    uint64_t index_device;
    uint64_t index_inode;
    int64_t index_mtime_sec;
    int64_t index_mtime_nsec;
    uint32_t absolute_page[IMX6_MACRONIX_LOGICAL_OOB_SAMPLE_COUNT];
    uint64_t stream_offset[IMX6_MACRONIX_LOGICAL_OOB_SAMPLE_COUNT];
    uint32_t entry_count;
    bool enabled;
} IMX6MacronixNANDLogicalOOBView;

/*
 * Separately opt-in replay of the one byte U-Boot marker probe.  This map is
 * derived from Run0005's Linux MTD OOB observations and health ioctl output;
 * it intentionally carries neither raw OOB/ECC nor main-area NAND data.
 */
#define IMX6_MACRONIX_UBOOT_MARKER_BLOCK_COUNT 2048
typedef struct IMX6MacronixNANDUBootMarkerView {
    uint8_t marker[IMX6_MACRONIX_UBOOT_MARKER_BLOCK_COUNT];
    uint8_t block_health[IMX6_MACRONIX_UBOOT_MARKER_BLOCK_COUNT];
    bool enabled;
    uint64_t map_size;
    uint64_t map_device;
    uint64_t map_inode;
    int64_t map_mtime_sec;
    int64_t map_mtime_nsec;
} IMX6MacronixNANDUBootMarkerView;

typedef enum IMX6MacronixMediaStatus {
    IMX6_MACRONIX_MEDIA_OK,
    IMX6_MACRONIX_MEDIA_UNAVAILABLE,
    IMX6_MACRONIX_MEDIA_UNBACKED,
    IMX6_MACRONIX_MEDIA_OUT_OF_RANGE,
    IMX6_MACRONIX_MEDIA_IO_ERROR,
    IMX6_MACRONIX_MEDIA_COVERAGE_UNKNOWN,
    IMX6_MACRONIX_MEDIA_LOGICAL_MARKER_REJECTED,
    IMX6_MACRONIX_MEDIA_LOGICAL_OOB_REJECTED,
    IMX6_MACRONIX_MEDIA_UBOOT_MARKER_REJECTED,
} IMX6MacronixMediaStatus;

typedef struct IMX6MacronixNANDState {
    IMX6MacronixPhase phase;
    /* Kept in the established migration format for the READ ID response. */
    uint8_t response[5];
    uint8_t response_length;
    uint8_t response_offset;
    uint16_t parameter_page_offset;
    uint8_t page_address[5];
    uint8_t page_address_count;
    uint16_t page_column;
    uint16_t page_column_cursor;
    uint32_t page_number;
    uint8_t cache_address_count;
    uint32_t cache_next_page;
    bool cache_valid;
    bool cache_pending;
    /* A 31 has established a sequential stream; 3f may end only that stream. */
    bool cache_stream_active;
    /* The current data output was selected by the final 3f cache command. */
    bool cache_final_output;
    bool page_media_unavailable;
    uint8_t page_media_status;
    bool ready;
    int64_t ready_deadline_ns;
    /* NAND status register state: 1 means the physical WP# pin is asserted. */
    bool write_protected;
    bool last_operation_failed;
} IMX6MacronixNANDState;

bool imx6_macronix_nand_media_open(IMX6MacronixNANDMedia *media,
                                   const char *path, const char *coverage_path,
                                   Error **errp);
void imx6_macronix_nand_media_close(IMX6MacronixNANDMedia *media);
bool imx6_macronix_nand_media_matches(const IMX6MacronixNANDMedia *media,
                                      uint64_t size, uint64_t device,
                                      uint64_t inode, int64_t mtime_sec,
                                      int64_t mtime_nsec);
bool imx6_macronix_nand_media_coverage_matches(const IMX6MacronixNANDMedia *media,
                                               uint64_t size, uint64_t device,
                                               uint64_t inode, int64_t mtime_sec,
                                               int64_t mtime_nsec);
bool imx6_macronix_nand_virtual_chip_open(IMX6MacronixNANDVirtualChip *chip,
                                          const char *path, bool ram_cache,
                                          Error **errp);
void imx6_macronix_nand_virtual_chip_close(IMX6MacronixNANDVirtualChip *chip);
bool imx6_macronix_nand_virtual_chip_matches(
    const IMX6MacronixNANDVirtualChip *chip, uint64_t size,
    uint64_t device, uint64_t inode, int64_t mtime_sec, int64_t mtime_nsec);
bool imx6_macronix_nand_virtual_chip_read_main(
    IMX6MacronixNANDState *nand, IMX6MacronixNANDVirtualChip *chip,
    uint8_t *bytes, uint32_t length);
bool imx6_macronix_nand_virtual_chip_read_page(
    IMX6MacronixNANDState *nand, IMX6MacronixNANDVirtualChip *chip,
    uint8_t payload[IMX6_MACRONIX_PAGE_MAIN_SIZE]);
bool imx6_macronix_nand_virtual_chip_program_page(
    IMX6MacronixNANDState *nand, IMX6MacronixNANDVirtualChip *chip,
    const uint8_t payload[IMX6_MACRONIX_PAGE_MAIN_SIZE], uint8_t auxiliary0);
bool imx6_macronix_nand_virtual_chip_auxiliary0(
    const IMX6MacronixNANDVirtualChip *chip, uint32_t page, uint8_t *value);
bool imx6_macronix_nand_virtual_chip_erase_block(
    IMX6MacronixNANDState *nand, IMX6MacronixNANDVirtualChip *chip);
bool imx6_macronix_nand_logical_media_open(IMX6MacronixNANDLogicalMedia *media,
                                           const char *path,
                                           uint32_t start_page,
                                           const char *marker_map_path,
                                           bool marker_projection,
                                           Error **errp);
void imx6_macronix_nand_logical_media_close(IMX6MacronixNANDLogicalMedia *media);
bool imx6_macronix_nand_logical_media_matches(
    const IMX6MacronixNANDLogicalMedia *media, uint64_t size,
    uint64_t device, uint64_t inode, int64_t mtime_sec, int64_t mtime_nsec,
    uint32_t start_page, uint32_t page_count);
bool imx6_macronix_nand_logical_marker_map_matches(
    const IMX6MacronixNANDLogicalMedia *media, uint64_t size,
    uint64_t device, uint64_t inode, int64_t mtime_sec, int64_t mtime_nsec);
void imx6_macronix_nand_reset(IMX6MacronixNANDState *nand);
bool imx6_macronix_nand_command(IMX6MacronixNANDState *nand, uint8_t byte);
bool imx6_macronix_nand_address(IMX6MacronixNANDState *nand, uint8_t byte);
bool imx6_macronix_nand_read(IMX6MacronixNANDState *nand, uint8_t *bytes,
                             uint32_t length,
                             IMX6MacronixNANDMedia *media,
                             bool page_fixture_enabled);
bool imx6_macronix_nand_ready(IMX6MacronixNANDState *nand);
int64_t imx6_macronix_nand_ready_deadline_ns(const IMX6MacronixNANDState *nand);
bool imx6_macronix_nand_logical_read(IMX6MacronixNANDState *nand,
                                     IMX6MacronixNANDLogicalMedia *media,
                                     uint8_t payload[IMX6_MACRONIX_PAGE_MAIN_SIZE],
                                     uint8_t auxiliary[16]);
bool imx6_macronix_nand_logical_marker_read(
    IMX6MacronixNANDState *nand, IMX6MacronixNANDLogicalMedia *media,
    uint8_t *bytes, uint32_t length);
bool imx6_macronix_nand_logical_oob_open(IMX6MacronixNANDLogicalOOBView *view,
                                         const char *samples_path,
                                         const char *index_path,
                                         bool enabled, Error **errp);
void imx6_macronix_nand_logical_oob_close(IMX6MacronixNANDLogicalOOBView *view);
bool imx6_macronix_nand_logical_oob_matches(
    const IMX6MacronixNANDLogicalOOBView *view,
    uint64_t samples_size, uint64_t samples_device, uint64_t samples_inode,
    int64_t samples_mtime_sec, int64_t samples_mtime_nsec,
    uint64_t index_size, uint64_t index_device, uint64_t index_inode,
    int64_t index_mtime_sec, int64_t index_mtime_nsec);
bool imx6_macronix_nand_logical_oob_read(
    IMX6MacronixNANDState *nand, IMX6MacronixNANDLogicalOOBView *view,
    uint8_t *bytes, uint32_t length);
bool imx6_macronix_nand_uboot_marker_open(IMX6MacronixNANDUBootMarkerView *view,
                                          const char *map_path, bool enabled,
                                          Error **errp);
void imx6_macronix_nand_uboot_marker_close(IMX6MacronixNANDUBootMarkerView *view);
bool imx6_macronix_nand_uboot_marker_matches(
    const IMX6MacronixNANDUBootMarkerView *view, uint64_t size,
    uint64_t device, uint64_t inode, int64_t mtime_sec, int64_t mtime_nsec);
bool imx6_macronix_nand_uboot_marker_read(
    IMX6MacronixNANDState *nand, IMX6MacronixNANDUBootMarkerView *view,
    uint8_t *bytes, uint32_t length);

#endif
