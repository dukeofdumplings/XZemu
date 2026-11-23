/* Minimal Macronix MX30LF2G18AC model for the XDJ-XZ NAND bus. */
#include "qemu/osdep.h"
#include "hw/misc/imx6_macronix_nand.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/timer.h"

#define MACRONIX_AUX_MAGIC "XZBCAUX1"
#define MACRONIX_AUX_VERSION 1
#define MACRONIX_AUX_HEADER_BYTES 64
#define MACRONIX_AUX_VALID_BYTES (IMX6_MACRONIX_PAGE_COUNT / 8)
#define MACRONIX_AUX_FILE_BYTES \
    (MACRONIX_AUX_HEADER_BYTES + MACRONIX_AUX_VALID_BYTES + \
     IMX6_MACRONIX_PAGE_COUNT)

static void macronix_media_identity(IMX6MacronixNANDMedia *media,
                                    const struct stat *st)
{
    media->host_size = st->st_size;
    media->host_device = st->st_dev;
    media->host_inode = st->st_ino;
    media->host_mtime_sec = st->st_mtime;
#ifdef __APPLE__
    media->host_mtime_nsec = st->st_mtimespec.tv_nsec;
#else
    media->host_mtime_nsec = st->st_mtim.tv_nsec;
#endif
    media->host_pages = media->host_size / IMX6_MACRONIX_PAGE_TOTAL;
}

static void macronix_coverage_identity(IMX6MacronixNANDMedia *media,
                                       const struct stat *st)
{
    media->coverage_size = st->st_size;
    media->coverage_device = st->st_dev;
    media->coverage_inode = st->st_ino;
    media->coverage_mtime_sec = st->st_mtime;
#ifdef __APPLE__
    media->coverage_mtime_nsec = st->st_mtimespec.tv_nsec;
#else
    media->coverage_mtime_nsec = st->st_mtim.tv_nsec;
#endif
}

bool imx6_macronix_nand_media_open(IMX6MacronixNANDMedia *media,
                                   const char *path, const char *coverage_path,
                                   Error **errp)
{
    struct stat st;
    struct stat coverage_st;
    uint64_t expected_coverage_size;

    if (!path || !*path) {
        return true;
    }
    if (stat(path, &st) < 0) {
        error_setg_errno(errp, errno, "cannot stat NAND media '%s'", path);
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        error_setg(errp, "NAND media '%s' is not a regular read-only file", path);
        return false;
    }
    if (st.st_size <= 0 || st.st_size % IMX6_MACRONIX_PAGE_TOTAL ||
        st.st_size > IMX6_MACRONIX_MEDIA_SIZE) {
        error_setg(errp,
                   "NAND media '%s' is %" PRId64 " bytes; raw physical-page media "
                   "must be a non-empty multiple of %u bytes, at most %" PRIu64,
                   path, (int64_t)st.st_size, IMX6_MACRONIX_PAGE_TOTAL,
                   IMX6_MACRONIX_MEDIA_SIZE);
        return false;
    }
    media->file = g_fopen(path, "rb");
    if (!media->file) {
        error_setg_errno(errp, errno, "cannot open NAND media '%s' read-only", path);
        return false;
    }
    macronix_media_identity(media, &st);
    if (coverage_path && *coverage_path) {
        if (stat(coverage_path, &coverage_st) < 0) {
            error_setg_errno(errp, errno, "cannot stat NAND coverage '%s'", coverage_path);
            imx6_macronix_nand_media_close(media);
            return false;
        }
        expected_coverage_size = (media->host_size + 7) / 8;
        if (!S_ISREG(coverage_st.st_mode) || coverage_st.st_size != expected_coverage_size) {
            error_setg(errp, "NAND coverage '%s' must be a regular %" PRIu64 "-byte file",
                       coverage_path, expected_coverage_size);
            imx6_macronix_nand_media_close(media);
            return false;
        }
        media->coverage_file = g_fopen(coverage_path, "rb");
        if (!media->coverage_file) {
            error_setg_errno(errp, errno, "cannot open NAND coverage '%s' read-only", coverage_path);
            imx6_macronix_nand_media_close(media);
            return false;
        }
        macronix_coverage_identity(media, &coverage_st);
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "XZ_NAND: media attached format=raw-physical-page bytes=%" PRIu64
                  " pages=%u path=%s\n", media->host_size, media->host_pages, path);
    return true;
}

void imx6_macronix_nand_media_close(IMX6MacronixNANDMedia *media)
{
    if (media->coverage_file) {
        fclose(media->coverage_file);
    }
    if (media->file) {
        fclose(media->file);
    }
    memset(media, 0, sizeof(*media));
}

bool imx6_macronix_nand_media_matches(const IMX6MacronixNANDMedia *media,
                                      uint64_t size, uint64_t device,
                                      uint64_t inode, int64_t mtime_sec,
                                      int64_t mtime_nsec)
{
    return media->file && media->host_size == size &&
           media->host_device == device && media->host_inode == inode &&
           media->host_mtime_sec == mtime_sec &&
           media->host_mtime_nsec == mtime_nsec;
}

bool imx6_macronix_nand_media_coverage_matches(const IMX6MacronixNANDMedia *media,
                                               uint64_t size, uint64_t device,
                                               uint64_t inode, int64_t mtime_sec,
                                               int64_t mtime_nsec)
{
    return media->coverage_file && media->coverage_size == size &&
           media->coverage_device == device && media->coverage_inode == inode &&
           media->coverage_mtime_sec == mtime_sec &&
           media->coverage_mtime_nsec == mtime_nsec;
}

static void macronix_virtual_chip_identity(IMX6MacronixNANDVirtualChip *chip,
                                           const struct stat *st)
{
    chip->host_size = st->st_size;
    chip->host_device = st->st_dev;
    chip->host_inode = st->st_ino;
    chip->host_mtime_sec = st->st_mtime;
#ifdef __APPLE__
    chip->host_mtime_nsec = st->st_mtimespec.tv_nsec;
#else
    chip->host_mtime_nsec = st->st_mtim.tv_nsec;
#endif
}

static bool macronix_virtual_chip_refresh_identity(IMX6MacronixNANDVirtualChip *chip)
{
    struct stat st;

    if (!chip->file || fstat(fileno(chip->file), &st) < 0) {
        return false;
    }
    macronix_virtual_chip_identity(chip, &st);
    return true;
}

static bool macronix_virtual_chip_aux_sync(FILE *file)
{
    return file && !fflush(file) && !fsync(fileno(file));
}

static void macronix_virtual_chip_aux_header(const IMX6MacronixNANDVirtualChip *chip,
                                             uint8_t header[MACRONIX_AUX_HEADER_BYTES])
{
    memset(header, 0, MACRONIX_AUX_HEADER_BYTES);
    memcpy(header, MACRONIX_AUX_MAGIC, 8);
    stl_le_p(header + 8, MACRONIX_AUX_VERSION);
    stl_le_p(header + 12, IMX6_MACRONIX_PAGE_COUNT);
    stq_le_p(header + 16, chip->host_size);
    stq_le_p(header + 24, chip->host_device);
    stq_le_p(header + 32, chip->host_inode);
    stq_le_p(header + 40, chip->host_mtime_sec);
    stq_le_p(header + 48, chip->host_mtime_nsec);
}

static bool macronix_virtual_chip_aux_header_matches(
    const IMX6MacronixNANDVirtualChip *chip,
    const uint8_t header[MACRONIX_AUX_HEADER_BYTES])
{
    return !memcmp(header, MACRONIX_AUX_MAGIC, 8) &&
           ldl_le_p(header + 8) == MACRONIX_AUX_VERSION &&
           ldl_le_p(header + 12) == IMX6_MACRONIX_PAGE_COUNT &&
           ldq_le_p(header + 16) == chip->host_size &&
           ldq_le_p(header + 24) == chip->host_device &&
           ldq_le_p(header + 32) == chip->host_inode &&
           (int64_t)ldq_le_p(header + 40) == chip->host_mtime_sec &&
           (int64_t)ldq_le_p(header + 48) == chip->host_mtime_nsec;
}

static bool macronix_virtual_chip_aux_write_header(IMX6MacronixNANDVirtualChip *chip)
{
    uint8_t header[MACRONIX_AUX_HEADER_BYTES];

    if (!chip->aux_file) {
        return true;
    }
    macronix_virtual_chip_aux_header(chip, header);
    if (fseeko(chip->aux_file, 0, SEEK_SET) ||
        fwrite(header, 1, sizeof(header), chip->aux_file) != sizeof(header) ||
        !macronix_virtual_chip_aux_sync(chip->aux_file)) {
        clearerr(chip->aux_file);
        return false;
    }
    return true;
}

static bool macronix_virtual_chip_aux_create(IMX6MacronixNANDVirtualChip *chip)
{
    uint8_t zeroes[4096] = { 0 };
    size_t remaining;

    if (chip->aux_file) {
        return true;
    }
    chip->aux_file = g_fopen(chip->aux_path, "w+b");
    if (!chip->aux_file || !macronix_virtual_chip_aux_write_header(chip)) {
        if (chip->aux_file) {
            fclose(chip->aux_file);
            chip->aux_file = NULL;
        }
        return false;
    }
    remaining = MACRONIX_AUX_VALID_BYTES + IMX6_MACRONIX_PAGE_COUNT;
    while (remaining) {
        size_t count = MIN(remaining, sizeof(zeroes));

        if (fwrite(zeroes, 1, count, chip->aux_file) != count) {
            clearerr(chip->aux_file);
            fclose(chip->aux_file);
            chip->aux_file = NULL;
            return false;
        }
        remaining -= count;
    }
    if (!macronix_virtual_chip_aux_sync(chip->aux_file)) {
        clearerr(chip->aux_file);
        fclose(chip->aux_file);
        chip->aux_file = NULL;
        return false;
    }
    return true;
}

static bool macronix_virtual_chip_aux_update_identity(IMX6MacronixNANDVirtualChip *chip)
{
    return macronix_virtual_chip_refresh_identity(chip) &&
           macronix_virtual_chip_aux_write_header(chip);
}

static bool macronix_virtual_chip_aux_store(IMX6MacronixNANDVirtualChip *chip,
                                             uint32_t page, uint8_t value)
{
    size_t byte = page / 8;
    uint8_t valid;
    off_t value_offset = MACRONIX_AUX_HEADER_BYTES + MACRONIX_AUX_VALID_BYTES + page;
    off_t valid_offset = MACRONIX_AUX_HEADER_BYTES + byte;

    if (page >= IMX6_MACRONIX_PAGE_COUNT ||
        !macronix_virtual_chip_aux_create(chip)) {
        return false;
    }
    valid = chip->aux_valid[byte] | (1u << (page & 7));
    if (fseeko(chip->aux_file, value_offset, SEEK_SET) ||
        fwrite(&value, 1, 1, chip->aux_file) != 1 ||
        fseeko(chip->aux_file, valid_offset, SEEK_SET) ||
        fwrite(&valid, 1, 1, chip->aux_file) != 1 ||
        !macronix_virtual_chip_aux_sync(chip->aux_file)) {
        clearerr(chip->aux_file);
        return false;
    }
    chip->aux_value[page] = value;
    chip->aux_valid[byte] = valid;
    return true;
}

static bool macronix_virtual_chip_aux_invalidate_block(
    IMX6MacronixNANDVirtualChip *chip, uint32_t first_page)
{
    size_t byte = first_page / 8;
    uint8_t cleared[64 / 8] = { 0 };

    if (!chip->aux_file) {
        return true;
    }
    if (fseeko(chip->aux_file, MACRONIX_AUX_HEADER_BYTES + byte, SEEK_SET) ||
        fwrite(cleared, 1, sizeof(cleared), chip->aux_file) != sizeof(cleared) ||
        !macronix_virtual_chip_aux_sync(chip->aux_file)) {
        clearerr(chip->aux_file);
        return false;
    }
    memset(chip->aux_valid + byte, 0, sizeof(cleared));
    memset(chip->aux_value + first_page, 0, 64);
    return true;
}

bool imx6_macronix_nand_virtual_chip_open(IMX6MacronixNANDVirtualChip *chip,
                                          const char *path, bool ram_cache,
                                          Error **errp)
{
    struct stat st;
    struct stat aux_st;
    uint8_t header[MACRONIX_AUX_HEADER_BYTES];
    const uint64_t expected = (uint64_t)IMX6_MACRONIX_PAGE_COUNT *
                              IMX6_MACRONIX_PAGE_MAIN_SIZE;

    if (!path || !*path) {
        return true;
    }
    if (stat(path, &st) < 0) {
        error_setg_errno(errp, errno, "cannot stat virtual NAND chip '%s'", path);
        return false;
    }
    if (!S_ISREG(st.st_mode) || st.st_size != expected) {
        error_setg(errp, "virtual NAND chip '%s' must be a regular exactly-%" PRIu64
                   "-byte 2048-byte-main-area image", path, expected);
        return false;
    }
    chip->file = g_fopen(path, "r+b");
    if (!chip->file) {
        error_setg_errno(errp, errno, "cannot open virtual NAND chip '%s' read/write", path);
        return false;
    }
    macronix_virtual_chip_identity(chip, &st);
    chip->program_count = g_new0(uint8_t, IMX6_MACRONIX_PAGE_COUNT);
    chip->aux_path = g_strdup_printf("%s.bchaux", path);
    chip->aux_valid = g_new0(uint8_t, MACRONIX_AUX_VALID_BYTES);
    chip->aux_value = g_new0(uint8_t, IMX6_MACRONIX_PAGE_COUNT);
    if (stat(chip->aux_path, &aux_st) == 0) {
        if (!S_ISREG(aux_st.st_mode) || aux_st.st_size != MACRONIX_AUX_FILE_BYTES) {
            error_setg(errp, "virtual NAND BCH auxiliary sidecar '%s' has invalid format",
                       chip->aux_path);
            imx6_macronix_nand_virtual_chip_close(chip);
            return false;
        }
        chip->aux_file = g_fopen(chip->aux_path, "r+b");
        if (!chip->aux_file ||
            fread(header, 1, sizeof(header), chip->aux_file) != sizeof(header) ||
            !macronix_virtual_chip_aux_header_matches(chip, header) ||
            fread(chip->aux_valid, 1, MACRONIX_AUX_VALID_BYTES, chip->aux_file) !=
                MACRONIX_AUX_VALID_BYTES ||
            fread(chip->aux_value, 1, IMX6_MACRONIX_PAGE_COUNT, chip->aux_file) !=
                IMX6_MACRONIX_PAGE_COUNT) {
            error_setg(errp, "virtual NAND BCH auxiliary sidecar '%s' does not match its main-area image",
                       chip->aux_path);
            if (chip->aux_file) {
                clearerr(chip->aux_file);
            }
            imx6_macronix_nand_virtual_chip_close(chip);
            return false;
        }
    } else if (errno != ENOENT) {
        error_setg_errno(errp, errno, "cannot stat virtual NAND BCH auxiliary sidecar '%s'",
                         chip->aux_path);
        imx6_macronix_nand_virtual_chip_close(chip);
        return false;
    }
    chip->ram_cache_enabled = ram_cache;
    if (ram_cache) {
        chip->ram_cache = g_malloc(expected);
        if (fread(chip->ram_cache, 1, expected, chip->file) != expected) {
            error_setg_errno(errp, errno, "cannot cache virtual NAND chip '%s'", path);
            clearerr(chip->file);
            imx6_macronix_nand_virtual_chip_close(chip);
            return false;
        }
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "XZ_NAND: virtual-chip attached format=main-area-v1 bytes=%" PRIu64
                  " pages=%u cache=%u bchaux=%s path=%s\n", expected,
                  IMX6_MACRONIX_PAGE_COUNT, ram_cache,
                  chip->aux_file ? "attached" : "legacy-none", path);
    return true;
}

void imx6_macronix_nand_virtual_chip_close(IMX6MacronixNANDVirtualChip *chip)
{
    if (chip->aux_file) {
        fclose(chip->aux_file);
    }
    if (chip->file) {
        fclose(chip->file);
    }
    g_free(chip->aux_path);
    g_free(chip->aux_valid);
    g_free(chip->aux_value);
    g_free(chip->ram_cache);
    g_free(chip->program_count);
    memset(chip, 0, sizeof(*chip));
}

bool imx6_macronix_nand_virtual_chip_auxiliary0(
    const IMX6MacronixNANDVirtualChip *chip, uint32_t page, uint8_t *value)
{
    if (!chip || !value || !chip->aux_file || page >= IMX6_MACRONIX_PAGE_COUNT ||
        !(chip->aux_valid[page / 8] & (1u << (page & 7)))) {
        return false;
    }
    *value = chip->aux_value[page];
    return true;
}

bool imx6_macronix_nand_virtual_chip_matches(
    const IMX6MacronixNANDVirtualChip *chip, uint64_t size,
    uint64_t device, uint64_t inode, int64_t mtime_sec, int64_t mtime_nsec)
{
    return chip->file && chip->host_size == size &&
           chip->host_device == device && chip->host_inode == inode &&
           chip->host_mtime_sec == mtime_sec &&
           chip->host_mtime_nsec == mtime_nsec;
}

bool imx6_macronix_nand_virtual_chip_read_main(
    IMX6MacronixNANDState *nand, IMX6MacronixNANDVirtualChip *chip,
    uint8_t *bytes, uint32_t length)
{
    uint64_t offset;

    if (!chip || !chip->file || !bytes ||
        nand->phase != IMX6_MACRONIX_PAGE_READ_DATA ||
        !imx6_macronix_nand_ready(nand) ||
        nand->page_column_cursor >= IMX6_MACRONIX_PAGE_MAIN_SIZE ||
        length > IMX6_MACRONIX_PAGE_MAIN_SIZE - nand->page_column_cursor) {
        nand->page_media_status = IMX6_MACRONIX_MEDIA_OUT_OF_RANGE;
        return false;
    }
    offset = (uint64_t)nand->page_number * IMX6_MACRONIX_PAGE_MAIN_SIZE +
             nand->page_column_cursor;
    if (chip->ram_cache) {
        memcpy(bytes, chip->ram_cache + offset, length);
    } else if (fseeko(chip->file, offset, SEEK_SET) ||
               fread(bytes, 1, length, chip->file) != length) {
        nand->page_media_status = IMX6_MACRONIX_MEDIA_IO_ERROR;
        clearerr(chip->file);
        return false;
    }
    nand->page_column_cursor += length;
    nand->page_media_status = IMX6_MACRONIX_MEDIA_OK;
    return true;
}

bool imx6_macronix_nand_virtual_chip_read_page(
    IMX6MacronixNANDState *nand, IMX6MacronixNANDVirtualChip *chip,
    uint8_t payload[IMX6_MACRONIX_PAGE_MAIN_SIZE])
{
    uint16_t saved_column = nand->page_column_cursor;

    nand->page_column_cursor = 0;
    if (!imx6_macronix_nand_virtual_chip_read_main(nand, chip, payload,
                                                   IMX6_MACRONIX_PAGE_MAIN_SIZE)) {
        nand->page_column_cursor = saved_column;
        return false;
    }
    nand->page_column_cursor = saved_column;
    return true;
}

bool imx6_macronix_nand_virtual_chip_program_page(
    IMX6MacronixNANDState *nand, IMX6MacronixNANDVirtualChip *chip,
    const uint8_t payload[IMX6_MACRONIX_PAGE_MAIN_SIZE], uint8_t auxiliary0)
{
    uint8_t current[IMX6_MACRONIX_PAGE_MAIN_SIZE];
    uint64_t offset;
    size_t index;

    if (!chip || !chip->file || !payload ||
        nand->phase != IMX6_MACRONIX_PAGE_PROGRAM_DATA ||
        nand->page_column != 0 || nand->page_number >= IMX6_MACRONIX_PAGE_COUNT ||
        chip->program_count[nand->page_number] >= 4) {
        return false;
    }
    if (chip->ram_cache) {
        memcpy(current, chip->ram_cache + (uint64_t)nand->page_number *
               IMX6_MACRONIX_PAGE_MAIN_SIZE, sizeof(current));
    } else {
        offset = (uint64_t)nand->page_number * IMX6_MACRONIX_PAGE_MAIN_SIZE;
        if (fseeko(chip->file, offset, SEEK_SET) ||
            fread(current, 1, sizeof(current), chip->file) != sizeof(current)) {
            clearerr(chip->file);
            return false;
        }
    }
    for (index = 0; index < sizeof(current); index++) {
        current[index] &= payload[index];
    }
    if (!macronix_virtual_chip_aux_store(chip, nand->page_number, auxiliary0)) {
        return false;
    }
    offset = (uint64_t)nand->page_number * IMX6_MACRONIX_PAGE_MAIN_SIZE;
    if (fseeko(chip->file, offset, SEEK_SET) ||
        fwrite(current, 1, sizeof(current), chip->file) != sizeof(current) ||
        fflush(chip->file) || fsync(fileno(chip->file))) {
        clearerr(chip->file);
        return false;
    }
    if (chip->ram_cache) {
        memcpy(chip->ram_cache + offset, current, sizeof(current));
    }
    chip->program_count[nand->page_number]++;
    if (!macronix_virtual_chip_aux_update_identity(chip)) {
        return false;
    }
    nand->last_operation_failed = false;
    nand->ready = false;
    nand->ready_deadline_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 700000;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "XZ_NAND: virtual-chip-program page=%u block=%u count=%u busy-ns=700000\n",
                  nand->page_number, nand->page_number / 64,
                  chip->program_count[nand->page_number]);
    return true;
}

bool imx6_macronix_nand_virtual_chip_erase_block(
    IMX6MacronixNANDState *nand, IMX6MacronixNANDVirtualChip *chip)
{
    uint8_t *erased;
    uint32_t page, first_page;
    uint64_t offset;

    if (!chip || !chip->file || nand->phase != IMX6_MACRONIX_BLOCK_ERASE_CONFIRM) {
        return false;
    }
    first_page = nand->page_number & ~UINT32_C(63);
    if (first_page >= IMX6_MACRONIX_PAGE_COUNT) {
        return false;
    }
    erased = g_malloc(IMX6_MACRONIX_PAGE_MAIN_SIZE * 64);
    memset(erased, 0xff, IMX6_MACRONIX_PAGE_MAIN_SIZE * 64);
    offset = (uint64_t)first_page * IMX6_MACRONIX_PAGE_MAIN_SIZE;
    if (fseeko(chip->file, offset, SEEK_SET) ||
        fwrite(erased, 1, IMX6_MACRONIX_PAGE_MAIN_SIZE * 64, chip->file) !=
        IMX6_MACRONIX_PAGE_MAIN_SIZE * 64 || fflush(chip->file) ||
        fsync(fileno(chip->file))) {
        clearerr(chip->file);
        g_free(erased);
        return false;
    }
    if (chip->ram_cache) {
        memcpy(chip->ram_cache + offset, erased, IMX6_MACRONIX_PAGE_MAIN_SIZE * 64);
    }
    g_free(erased);
    for (page = first_page; page < first_page + 64; page++) {
        chip->program_count[page] = 0;
    }
    if (!macronix_virtual_chip_aux_invalidate_block(chip, first_page) ||
        !macronix_virtual_chip_aux_update_identity(chip)) {
        return false;
    }
    nand->last_operation_failed = false;
    nand->ready = false;
    nand->ready_deadline_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 10000000;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "XZ_NAND: virtual-chip-erase block=%u first-page=%u busy-ns=10000000\n",
                  first_page / 64, first_page);
    return true;
}

static bool macronix_marker_map_hex(const char *value)
{
    size_t i;

    if (strlen(value) != 64) {
        return false;
    }
    for (i = 0; i < 64; i++) {
        if (!g_ascii_isxdigit(value[i])) {
            return false;
        }
    }
    return true;
}

static bool macronix_logical_marker_map_open(IMX6MacronixNANDLogicalMedia *media,
                                             const char *path, Error **errp)
{
    static const char *const headers[] = {
        "XZ_LOGICAL_BLOCK_HEALTH/1",
        "archive_identity=run-0004",
        NULL, NULL,
        "partition=env",
        "start_page=2048",
        "page_count=512",
        "erase_bytes=131072",
        "block_count=8",
    };
    FILE *file;
    struct stat st;
    char line[160];
    char status[16];
    unsigned int index, page;
    unsigned int i;

    if (stat(path, &st) < 0 || !S_ISREG(st.st_mode)) {
        error_setg_errno(errp, errno, "cannot stat logical marker map '%s'", path);
        return false;
    }
    file = g_fopen(path, "rb");
    if (!file) {
        error_setg_errno(errp, errno, "cannot open logical marker map '%s'", path);
        return false;
    }
    for (i = 0; i < G_N_ELEMENTS(headers); i++) {
        if (!fgets(line, sizeof(line), file)) {
            error_setg(errp, "logical marker map '%s' is truncated", path);
            goto fail;
        }
        line[strcspn(line, "\r\n")] = '\0';
        if (i == 2 || i == 3) {
            const char *prefix = i == 2 ? "logical_source_sha256=" :
                                          "block_health_sha256=";
            if (!g_str_has_prefix(line, prefix) ||
                !macronix_marker_map_hex(line + strlen(prefix))) {
                error_setg(errp, "logical marker map '%s' has invalid provenance hash", path);
                goto fail;
            }
        } else if (strcmp(line, headers[i])) {
            error_setg(errp, "logical marker map '%s' has unsupported geometry/provenance", path);
            goto fail;
        }
    }
    for (i = 0; i < G_N_ELEMENTS(media->block_health); i++) {
        if (!fgets(line, sizeof(line), file) ||
            sscanf(line, "%u\t%u\t%15s", &index, &page, status) != 3 ||
            index != i || page != 2048 + i * 64 ||
            (strcmp(status, "GOOD") && strcmp(status, "BAD"))) {
            error_setg(errp, "logical marker map '%s' has invalid block entry", path);
            goto fail;
        }
        media->block_health[i] = !strcmp(status, "BAD");
    }
    if (fgets(line, sizeof(line), file)) {
        error_setg(errp, "logical marker map '%s' has extra entries", path);
        goto fail;
    }
    fclose(file);
    media->marker_map_configured = true;
    media->marker_map_size = st.st_size;
    media->marker_map_device = st.st_dev;
    media->marker_map_inode = st.st_ino;
    media->marker_map_mtime_sec = st.st_mtime;
#ifdef __APPLE__
    media->marker_map_mtime_nsec = st.st_mtimespec.tv_nsec;
#else
    media->marker_map_mtime_nsec = st.st_mtim.tv_nsec;
#endif
    return true;

fail:
    fclose(file);
    return false;
}

bool imx6_macronix_nand_logical_media_open(IMX6MacronixNANDLogicalMedia *media,
                                           const char *path,
                                           uint32_t start_page,
                                           const char *marker_map_path,
                                           bool marker_projection,
                                           Error **errp)
{
    struct stat st;

    if (!path || !*path) {
        if (marker_projection || (marker_map_path && *marker_map_path)) {
            error_setg(errp, "logical marker projection requires logical-nand-media");
            return false;
        }
        return true;
    }
    if (marker_projection != !!(marker_map_path && *marker_map_path)) {
        error_setg(errp, "logical marker projection requires both explicit opt-in and marker map");
        return false;
    }
    if (stat(path, &st) < 0) {
        error_setg_errno(errp, errno, "cannot stat logical NAND media '%s'", path);
        return false;
    }
    if (!S_ISREG(st.st_mode) || st.st_size <= 0 ||
        st.st_size % IMX6_MACRONIX_PAGE_MAIN_SIZE ||
        start_page >= IMX6_MACRONIX_PAGE_COUNT ||
        st.st_size / IMX6_MACRONIX_PAGE_MAIN_SIZE >
        IMX6_MACRONIX_PAGE_COUNT - start_page) {
        error_setg(errp,
                   "logical NAND media '%s' must be a non-empty 2048-byte-page file within the NAND address space",
                   path);
        return false;
    }
    media->file = g_fopen(path, "rb");
    if (!media->file) {
        error_setg_errno(errp, errno,
                         "cannot open logical NAND media '%s' read-only", path);
        return false;
    }
    media->host_size = st.st_size;
    media->host_device = st.st_dev;
    media->host_inode = st.st_ino;
    media->host_mtime_sec = st.st_mtime;
#ifdef __APPLE__
    media->host_mtime_nsec = st.st_mtimespec.tv_nsec;
#else
    media->host_mtime_nsec = st.st_mtim.tv_nsec;
#endif
    media->start_page = start_page;
    media->page_count = media->host_size / IMX6_MACRONIX_PAGE_MAIN_SIZE;

    /*
     * Logical media is explicitly read-only and its identity is checked on
     * migration.  Read it once now so a page delivery is only a bounded
     * memory copy, rather than a host fseek/fread pair on every NAND page.
     */
    media->payload_cache = g_malloc(media->host_size);
    if (fseeko(media->file, 0, SEEK_SET) ||
        fread(media->payload_cache, 1, media->host_size, media->file) !=
        media->host_size) {
        int saved_errno = errno;

        if (!saved_errno) {
            saved_errno = EIO;
        }
        clearerr(media->file);
        g_free(media->payload_cache);
        fclose(media->file);
        memset(media, 0, sizeof(*media));
        error_setg_errno(errp, saved_errno,
                         "cannot read logical NAND media '%s'", path);
        return false;
    }
    if (marker_projection) {
        if (start_page != 2048 || media->page_count != 512 ||
            !macronix_logical_marker_map_open(media, marker_map_path, errp)) {
            if (!*errp) {
                error_setg(errp, "logical marker projection only supports run-0004 env pages 2048-2559");
            }
            g_free(media->payload_cache);
            fclose(media->file);
            memset(media, 0, sizeof(*media));
            return false;
        }
        media->marker_projection = true;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "XZ_NAND: media attached format=logical-backed-main-only "
                  "bytes=%" PRIu64 " start-page=%u pages=%u path=%s\n",
                  media->host_size, media->start_page, media->page_count, path);
    qemu_log_mask(LOG_GUEST_ERROR,
                  "XZ_NAND: logical-media-cache=resident bytes=%" PRIu64
                  " start-page=%u\n",
                  media->host_size, media->start_page);
    return true;
}

void imx6_macronix_nand_logical_media_close(IMX6MacronixNANDLogicalMedia *media)
{
    g_free(media->payload_cache);
    if (media->file) {
        fclose(media->file);
    }
    memset(media, 0, sizeof(*media));
}

bool imx6_macronix_nand_logical_media_matches(
    const IMX6MacronixNANDLogicalMedia *media, uint64_t size,
    uint64_t device, uint64_t inode, int64_t mtime_sec, int64_t mtime_nsec,
    uint32_t start_page, uint32_t page_count)
{
    return media->file && media->host_size == size &&
           media->host_device == device && media->host_inode == inode &&
           media->host_mtime_sec == mtime_sec &&
           media->host_mtime_nsec == mtime_nsec &&
           media->start_page == start_page && media->page_count == page_count;
}

bool imx6_macronix_nand_logical_marker_map_matches(
    const IMX6MacronixNANDLogicalMedia *media, uint64_t size,
    uint64_t device, uint64_t inode, int64_t mtime_sec, int64_t mtime_nsec)
{
    return media->marker_map_configured && media->marker_projection &&
           media->marker_map_size == size && media->marker_map_device == device &&
           media->marker_map_inode == inode && media->marker_map_mtime_sec == mtime_sec &&
           media->marker_map_mtime_nsec == mtime_nsec;
}

static bool macronix_media_coverage_contains(IMX6MacronixNANDMedia *media,
                                             uint64_t offset, uint32_t length)
{
    g_autofree uint8_t *bits = NULL;
    uint64_t first_byte, last_byte;
    uint32_t i;

    if (!media->coverage_file) {
        return true;
    }
    first_byte = offset / 8;
    last_byte = (offset + length - 1) / 8;
    bits = g_malloc(last_byte - first_byte + 1);
    if (fseeko(media->coverage_file, first_byte, SEEK_SET) ||
        fread(bits, 1, last_byte - first_byte + 1, media->coverage_file) !=
        last_byte - first_byte + 1) {
        clearerr(media->coverage_file);
        return false;
    }
    for (i = 0; i < length; i++) {
        uint64_t index = offset + i;

        if (!(bits[index / 8 - first_byte] & (1U << (index & 7)))) {
            return false;
        }
    }
    return true;
}

static uint16_t macronix_onfi_crc16(const uint8_t *data, size_t length)
{
    uint16_t crc = 0x4f4e;
    unsigned int bit;

    while (length--) {
        crc ^= (uint16_t)*data++ << 8;
        for (bit = 0; bit < 8; bit++) {
            crc = (crc << 1) ^ ((crc & 0x8000) ? 0x8005 : 0);
        }
    }
    return crc;
}

static void macronix_parameter_page(uint8_t page[IMX6_MACRONIX_PARAM_PAGE_SIZE])
{
    uint16_t crc;

    memset(page, 0, IMX6_MACRONIX_PARAM_PAGE_SIZE);
    memcpy(page + 0, "ONFI", 4);
    page[4] = 0x02;
    page[6] = 0x18;
    page[8] = 0x3f;
    memcpy(page + 32, "MACRONIX    ", 12);
    memcpy(page + 44, "MX30LF2G18AC        ", 20);
    page[64] = 0xc2;
    page[80] = 0x00; page[81] = 0x08;
    page[84] = 0x40;
    page[86] = 0x00; page[87] = 0x02;
    page[90] = 0x10;
    page[92] = 0x40;
    page[96] = 0x00; page[97] = 0x08;
    page[100] = 0x01;
    page[101] = 0x23;
    page[102] = 0x01;
    page[103] = 0x28;
    page[105] = 0x01; page[106] = 0x05;
    page[107] = 0x01;
    page[108] = 0x01; page[109] = 0x03;
    page[110] = 0x04;
    page[112] = 0x04;
    page[113] = 0x01;
    page[114] = 0x0e;
    page[128] = 0x0a;
    page[129] = 0x3f;
    page[131] = 0x3f;
    page[133] = 0x58; page[134] = 0x02;
    page[135] = 0xac; page[136] = 0x0d;
    page[137] = 0x19;
    page[139] = 0x3c;
    crc = macronix_onfi_crc16(page, 254);
    page[254] = crc & 0xff;
    page[255] = crc >> 8;
}

static bool macronix_parameter_page_read(IMX6MacronixNANDState *nand,
                                         uint8_t *bytes, uint32_t length)
{
    uint8_t page[IMX6_MACRONIX_PARAM_PAGE_SIZE];
    uint32_t copied = 0;

    if (nand->parameter_page_offset > IMX6_MACRONIX_PARAM_PAGE_TOTAL ||
        length > IMX6_MACRONIX_PARAM_PAGE_TOTAL -
                 nand->parameter_page_offset) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_NAND: parameter-page-read-out-of-range offset=%u bytes=%u\n",
                      nand->parameter_page_offset, length);
        return false;
    }

    macronix_parameter_page(page);
    while (copied < length) {
        uint32_t page_offset = (nand->parameter_page_offset + copied) %
                               IMX6_MACRONIX_PARAM_PAGE_SIZE;
        uint32_t chunk = MIN(length - copied,
                             IMX6_MACRONIX_PARAM_PAGE_SIZE - page_offset);

        memcpy(bytes + copied, page + page_offset, chunk);
        copied += chunk;
    }
    return true;
}

static bool macronix_page_fixture_read(IMX6MacronixNANDState *nand,
                                       uint8_t *bytes, uint32_t length,
                                       bool page_fixture_enabled)
{
    uint32_t offset;
    uint32_t i;

    if (nand->page_column_cursor > IMX6_MACRONIX_PAGE_TOTAL ||
        length > IMX6_MACRONIX_PAGE_TOTAL - nand->page_column_cursor) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_NAND: page-read-out-of-range page=%u column=%u bytes=%u\n",
                      nand->page_number, nand->page_column_cursor, length);
        return false;
    }
    if (!page_fixture_enabled) {
        nand->page_media_unavailable = true;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_NAND: page-media-unavailable page=%u column=%u bytes=%u\n",
                      nand->page_number, nand->page_column_cursor, length);
        return false;
    }

    for (i = 0; i < length; i++) {
        offset = nand->page_column_cursor + i;
        /* Opt-in P6 test fixture; this is not XDJ-XZ NAND content. */
        bytes[i] = nand->page_number == 0 && offset == IMX6_MACRONIX_PAGE_MAIN_SIZE
                   ? 0xa5 : (0x5a ^ (offset & 0xff) ^ (nand->page_number & 0xff));
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "XZ_NAND: page-fixture response page=%u column=%u bytes=%u first=0x%02x\n",
                  nand->page_number, nand->page_column_cursor, length, bytes[0]);
    nand->page_column_cursor += length;
    return true;
}

static bool macronix_page_media_read(IMX6MacronixNANDState *nand,
                                     IMX6MacronixNANDMedia *media,
                                     uint8_t *bytes, uint32_t length)
{
    uint64_t offset;

    if (!media || !media->file) {
        nand->page_media_unavailable = true;
        nand->page_media_status = IMX6_MACRONIX_MEDIA_UNAVAILABLE;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_NAND: page-media-unavailable page=%u column=%u bytes=%u\n",
                      nand->page_number, nand->page_column_cursor, length);
        return false;
    }
    if (nand->page_number >= media->host_pages) {
        nand->page_media_status = IMX6_MACRONIX_MEDIA_UNBACKED;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_NAND: page-media-unbacked page=%u pages=%u column=%u bytes=%u\n",
                      nand->page_number, media->host_pages,
                      nand->page_column_cursor, length);
        return false;
    }
    offset = (uint64_t)nand->page_number * IMX6_MACRONIX_PAGE_TOTAL +
             nand->page_column_cursor;
    if (!macronix_media_coverage_contains(media, offset, length)) {
        nand->page_media_status = IMX6_MACRONIX_MEDIA_COVERAGE_UNKNOWN;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_NAND: page-media-coverage-unknown page=%u column=%u bytes=%u offset=%" PRIu64 "\n",
                      nand->page_number, nand->page_column_cursor, length, offset);
        return false;
    }
    if (fseeko(media->file, offset, SEEK_SET) ||
        fread(bytes, 1, length, media->file) != length) {
        nand->page_media_status = IMX6_MACRONIX_MEDIA_IO_ERROR;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_NAND: page-media-io-error page=%u column=%u bytes=%u offset=%" PRIu64 "\n",
                      nand->page_number, nand->page_column_cursor, length, offset);
        clearerr(media->file);
        return false;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "XZ_NAND: media response format=raw-physical-page page=%u column=%u "
                  "bytes=%u offset=%" PRIu64 " first=0x%02x\n",
                  nand->page_number, nand->page_column_cursor, length,
                  offset, bytes[0]);
    nand->page_column_cursor += length;
    return true;
}

void imx6_macronix_nand_reset(IMX6MacronixNANDState *nand)
{
    nand->phase = IMX6_MACRONIX_IDLE;
    nand->response_length = 0;
    nand->response_offset = 0;
    nand->parameter_page_offset = 0;
    memset(nand->page_address, 0, sizeof(nand->page_address));
    nand->page_address_count = 0;
    nand->page_column = 0;
    nand->page_column_cursor = 0;
    nand->page_number = 0;
    nand->cache_address_count = 0;
    nand->cache_next_page = 0;
    nand->cache_valid = false;
    nand->cache_pending = false;
    nand->cache_stream_active = false;
    nand->cache_final_output = false;
    nand->page_media_unavailable = false;
    nand->page_media_status = IMX6_MACRONIX_MEDIA_OK;
    nand->ready = true;
    nand->ready_deadline_ns = 0;
    nand->write_protected = false;
    nand->last_operation_failed = false;
}

static uint8_t macronix_status(const IMX6MacronixNANDState *nand, bool ready)
{
    uint8_t status = 0;

    /* MX30LF2G18AC: SR6 R/B#, SR5 controller ready, SR7 WP# (1=unprotected). */
    if (ready) {
        status |= 0x60;
    }
    if (!nand->write_protected) {
        status |= 0x80;
    }
    if (nand->last_operation_failed) {
        status |= 0x01;
    }
    return status;
}

bool imx6_macronix_nand_ready(IMX6MacronixNANDState *nand)
{
    if (!nand->ready && qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) >=
        nand->ready_deadline_ns) {
        nand->ready = true;
        nand->cache_pending = false;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_NAND: ready page=%u cache_valid=%u\n",
                      nand->cache_valid ? nand->cache_next_page : nand->page_number,
                      nand->cache_valid);
    }
    return nand->ready;
}

int64_t imx6_macronix_nand_ready_deadline_ns(const IMX6MacronixNANDState *nand)
{
    return nand->ready_deadline_ns;
}

bool imx6_macronix_nand_logical_read(IMX6MacronixNANDState *nand,
                                     IMX6MacronixNANDLogicalMedia *media,
                                     uint8_t payload[IMX6_MACRONIX_PAGE_MAIN_SIZE],
                                     uint8_t auxiliary[16])
{
    uint32_t output_page;
    uint64_t offset;

    if (!media || !media->file || nand->phase != IMX6_MACRONIX_PAGE_READ_DATA ||
        !imx6_macronix_nand_ready(nand)) {
        return false;
    }
    output_page = nand->page_number;
    if (output_page < media->start_page ||
        output_page - media->start_page >= media->page_count) {
        nand->page_media_status = IMX6_MACRONIX_MEDIA_UNBACKED;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_NAND: logical-media-unbacked output-page=%u start-page=%u pages=%u\n",
                      output_page, media->start_page, media->page_count);
        return false;
    }
    offset = (uint64_t)(output_page - media->start_page) *
             IMX6_MACRONIX_PAGE_MAIN_SIZE;
    if (media->payload_cache) {
        memcpy(payload, media->payload_cache + offset,
               IMX6_MACRONIX_PAGE_MAIN_SIZE);
    } else if (fseeko(media->file, offset, SEEK_SET) ||
               fread(payload, 1, IMX6_MACRONIX_PAGE_MAIN_SIZE, media->file) !=
               IMX6_MACRONIX_PAGE_MAIN_SIZE) {
        nand->page_media_status = IMX6_MACRONIX_MEDIA_IO_ERROR;
        clearerr(media->file);
        return false;
    }

    /*
     * This auxiliary buffer is a virtual driver-contract value, never raw
     * OOB.  Mirroring the logical marker makes the source driver's marker
     * swap an identity transform without asserting a physical marker value.
     */
    memset(auxiliary, 0xff, 12);
    auxiliary[0] = payload[1999];
    memset(auxiliary + 12, 0x00, 4); /* four clean chunk statuses */
    qemu_log_mask(LOG_GUEST_ERROR,
                  "XZ_NAND: logical-backed response output-page=%u bytes=%u "
                  "aux=virtual-clean metadata=virtual\n",
                  output_page, IMX6_MACRONIX_PAGE_MAIN_SIZE);

    if (nand->cache_final_output) {
        /*
         * 3f selects the already-prefetched final row for one last data-out.
         * The logical BCH contract consumes the complete 2048-byte main area
         * in this call, so do not leave it available for a second delivery.
         */
        nand->phase = IMX6_MACRONIX_IDLE;
        nand->cache_valid = false;
        nand->cache_pending = false;
        nand->cache_stream_active = false;
        nand->cache_final_output = false;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_NAND: final-cache-read consumed output-page=%u\n",
                      output_page);
        return true;
    }

    if (nand->cache_valid && nand->cache_next_page == output_page + 1) {
        nand->page_number = nand->cache_next_page;
        nand->page_column = 0;
        nand->page_column_cursor = 0;
        /*
         * The prefetched row has become the next cache output.  It remains
         * valid and may itself be followed by another 31/00/00 prefetch.
         * Clearing this bit made the first logical read work but rejected
         * the next authentic sequential-cache transition.
         */
        nand->cache_valid = true;
    }
    return true;
}

bool imx6_macronix_nand_logical_marker_read(
    IMX6MacronixNANDState *nand, IMX6MacronixNANDLogicalMedia *media,
    uint8_t *bytes, uint32_t length)
{
    uint32_t block;

    /*
     * This is intentionally not an OOB backend.  It implements only the
     * observed U-Boot marker transaction: read-confirmed DATA, page at an env
     * eraseblock boundary, column 2048, one byte.  The value is a canonical
     * logical GOOD/BAD projection, not a claim about unobserved physical OOB.
     */
    if (!media || !media->file || !media->marker_projection || !bytes ||
        nand->phase != IMX6_MACRONIX_PAGE_READ_DATA ||
        !imx6_macronix_nand_ready(nand) || length != 1 ||
        nand->page_column_cursor != IMX6_MACRONIX_PAGE_MAIN_SIZE ||
        nand->page_number < 2048 || nand->page_number >= 2560 ||
        (nand->page_number - 2048) % 64) {
        nand->page_media_status = IMX6_MACRONIX_MEDIA_LOGICAL_MARKER_REJECTED;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_NAND: logical-marker-provenance-rejected page=%u column=%u bytes=%u opt-in=%u\n",
                      nand->page_number, nand->page_column_cursor, length,
                      media && media->marker_projection);
        return false;
    }
    block = (nand->page_number - 2048) / 64;
    bytes[0] = media->block_health[block] ? 0x00 : 0xff;
    nand->page_column_cursor++;
    nand->page_media_status = IMX6_MACRONIX_MEDIA_OK;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "XZ_NAND: logical-marker-projection page=%u column=2048 "
                  "block=%u health=%s marker=0x%02x\n",
                  nand->page_number, block,
                  media->block_health[block] ? "BAD" : "GOOD", bytes[0]);
    return true;
}

static void macronix_logical_oob_identity(IMX6MacronixNANDLogicalOOBView *view,
                                          const struct stat *samples,
                                          const struct stat *index)
{
    view->samples_size = samples->st_size;
    view->samples_device = samples->st_dev;
    view->samples_inode = samples->st_ino;
    view->samples_mtime_sec = samples->st_mtime;
    view->index_size = index->st_size;
    view->index_device = index->st_dev;
    view->index_inode = index->st_ino;
    view->index_mtime_sec = index->st_mtime;
#ifdef __APPLE__
    view->samples_mtime_nsec = samples->st_mtimespec.tv_nsec;
    view->index_mtime_nsec = index->st_mtimespec.tv_nsec;
#else
    view->samples_mtime_nsec = samples->st_mtim.tv_nsec;
    view->index_mtime_nsec = index->st_mtim.tv_nsec;
#endif
}

void imx6_macronix_nand_logical_oob_close(IMX6MacronixNANDLogicalOOBView *view)
{
    if (view->samples_file) {
        fclose(view->samples_file);
    }
    memset(view, 0, sizeof(*view));
}

bool imx6_macronix_nand_logical_oob_open(IMX6MacronixNANDLogicalOOBView *view,
                                         const char *samples_path,
                                         const char *index_path,
                                         bool enabled, Error **errp)
{
    static const char header[] =
        "absolute_page\tblock_index\tpage_in_block\tstream_offset\tresult\treturned_oob_length\terrno";
    struct stat samples_st;
    struct stat index_st;
    FILE *index_file = NULL;
    char line[192];
    unsigned int expected, page, block, page_in_block, returned;
    uint64_t offset;
    int saved_errno;

    if (!enabled) {
        if ((samples_path && *samples_path) || (index_path && *index_path)) {
            error_setg(errp, "logical OOB view paths require explicit opt-in");
            return false;
        }
        return true;
    }
    if (!samples_path || !*samples_path || !index_path || !*index_path) {
        error_setg(errp, "logical OOB view requires both samples and index paths");
        return false;
    }
    if (stat(samples_path, &samples_st) < 0 || !S_ISREG(samples_st.st_mode) ||
        samples_st.st_size != IMX6_MACRONIX_LOGICAL_OOB_SAMPLE_COUNT *
                              IMX6_MACRONIX_PAGE_OOB_SIZE) {
        error_setg(errp, "logical OOB samples must be a regular 262144-byte file");
        return false;
    }
    if (stat(index_path, &index_st) < 0 || !S_ISREG(index_st.st_mode)) {
        error_setg(errp, "logical OOB index must be a regular file");
        return false;
    }
    view->samples_file = g_fopen(samples_path, "rb");
    index_file = g_fopen(index_path, "rb");
    if (!view->samples_file || !index_file) {
        saved_errno = errno;
        if (index_file) {
            fclose(index_file);
        }
        imx6_macronix_nand_logical_oob_close(view);
        error_setg_errno(errp, saved_errno, "cannot open logical OOB inputs read-only");
        return false;
    }
    if (!fgets(line, sizeof(line), index_file) ||
        strcmp(g_strchomp(line), header)) {
        error_setg(errp, "logical OOB index has an unsupported header");
        goto fail;
    }
    for (expected = 0; expected < IMX6_MACRONIX_LOGICAL_OOB_SAMPLE_COUNT;
         expected++) {
        char result[16];
        int request_errno;
        unsigned int expected_block = expected / 2;
        unsigned int expected_page_in_block = expected % 2;

        if (!fgets(line, sizeof(line), index_file) ||
            sscanf(line, "%u\t%u\t%u\t%" SCNu64 "\t%15s\t%u\t%d",
                   &page, &block, &page_in_block, &offset, result, &returned,
                   &request_errno) != 7 ||
            page != expected_block * 64 + expected_page_in_block ||
            block != expected_block || page_in_block != expected_page_in_block ||
            offset != (uint64_t)expected * IMX6_MACRONIX_PAGE_OOB_SIZE ||
            strcmp(result, "SUCCESS") || returned != IMX6_MACRONIX_PAGE_OOB_SIZE ||
            request_errno != 0) {
            error_setg(errp, "logical OOB index has unsupported coverage or offsets");
            goto fail;
        }
        view->absolute_page[expected] = page;
        view->stream_offset[expected] = offset;
    }
    if (fgets(line, sizeof(line), index_file)) {
        error_setg(errp, "logical OOB index has extra entries");
        goto fail;
    }
    fclose(index_file);
    view->entry_count = IMX6_MACRONIX_LOGICAL_OOB_SAMPLE_COUNT;
    view->enabled = true;
    macronix_logical_oob_identity(view, &samples_st, &index_st);
    qemu_log_mask(LOG_GUEST_ERROR,
                  "XZ_NAND: diagnostic OOB view attached format=linux-mtd-oob-view "
                  "samples=%u bytes=%" PRIu64 " path=%s\n",
                  view->entry_count, view->samples_size, samples_path);
    return true;

fail:
    fclose(index_file);
    imx6_macronix_nand_logical_oob_close(view);
    return false;
}

bool imx6_macronix_nand_logical_oob_matches(
    const IMX6MacronixNANDLogicalOOBView *view,
    uint64_t samples_size, uint64_t samples_device, uint64_t samples_inode,
    int64_t samples_mtime_sec, int64_t samples_mtime_nsec,
    uint64_t index_size, uint64_t index_device, uint64_t index_inode,
    int64_t index_mtime_sec, int64_t index_mtime_nsec)
{
    return view->enabled && view->samples_file &&
           view->samples_size == samples_size &&
           view->samples_device == samples_device &&
           view->samples_inode == samples_inode &&
           view->samples_mtime_sec == samples_mtime_sec &&
           view->samples_mtime_nsec == samples_mtime_nsec &&
           view->index_size == index_size && view->index_device == index_device &&
           view->index_inode == index_inode &&
           view->index_mtime_sec == index_mtime_sec &&
           view->index_mtime_nsec == index_mtime_nsec;
}

bool imx6_macronix_nand_logical_oob_read(
    IMX6MacronixNANDState *nand, IMX6MacronixNANDLogicalOOBView *view,
    uint8_t *bytes, uint32_t length)
{
    uint32_t index;

    if (!view || !view->enabled || !view->samples_file || !bytes ||
        nand->phase != IMX6_MACRONIX_PAGE_READ_DATA ||
        !imx6_macronix_nand_ready(nand) ||
        nand->page_column_cursor != IMX6_MACRONIX_PAGE_MAIN_SIZE ||
        length != IMX6_MACRONIX_PAGE_OOB_SIZE) {
        nand->page_media_status = IMX6_MACRONIX_MEDIA_LOGICAL_OOB_REJECTED;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_NAND: logical-oob-provenance-rejected page=%u column=%u bytes=%u opt-in=%u\n",
                      nand->page_number, nand->page_column_cursor, length,
                      view && view->enabled);
        return false;
    }
    for (index = 0; index < view->entry_count; index++) {
        if (view->absolute_page[index] == nand->page_number) {
            if (fseeko(view->samples_file, view->stream_offset[index], SEEK_SET) ||
                fread(bytes, 1, IMX6_MACRONIX_PAGE_OOB_SIZE,
                      view->samples_file) != IMX6_MACRONIX_PAGE_OOB_SIZE) {
                nand->page_media_status = IMX6_MACRONIX_MEDIA_IO_ERROR;
                clearerr(view->samples_file);
                qemu_log_mask(LOG_GUEST_ERROR,
                              "XZ_NAND: logical-oob-view-io-error page=%u\n",
                              nand->page_number);
                return false;
            }
            nand->page_column_cursor += IMX6_MACRONIX_PAGE_OOB_SIZE;
            nand->page_media_status = IMX6_MACRONIX_MEDIA_OK;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "XZ_NAND: logical-oob-view response page=%u column=2048 bytes=64 "
                          "view=linux-mtd-oob-not-raw first=0x%02x\n",
                          nand->page_number, bytes[0]);
            return true;
        }
    }
    nand->page_media_status = IMX6_MACRONIX_MEDIA_LOGICAL_OOB_REJECTED;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "XZ_NAND: logical-oob-provenance-rejected page=%u column=2048 bytes=64 "
                  "reason=unbacked\n", nand->page_number);
    return false;
}

static void macronix_uboot_marker_identity(IMX6MacronixNANDUBootMarkerView *view,
                                           const struct stat *st)
{
    view->map_size = st->st_size;
    view->map_device = st->st_dev;
    view->map_inode = st->st_ino;
    view->map_mtime_sec = st->st_mtime;
#ifdef __APPLE__
    view->map_mtime_nsec = st->st_mtimespec.tv_nsec;
#else
    view->map_mtime_nsec = st->st_mtim.tv_nsec;
#endif
}

void imx6_macronix_nand_uboot_marker_close(IMX6MacronixNANDUBootMarkerView *view)
{
    memset(view, 0, sizeof(*view));
}

bool imx6_macronix_nand_uboot_marker_open(IMX6MacronixNANDUBootMarkerView *view,
                                          const char *map_path, bool enabled,
                                          Error **errp)
{
    static const char *const headers[] = {
        "XZ_UBOOT_MARKER_PROJECTION/1",
        "source_kind=DERIVED_FROM_RUN0005",
        NULL, NULL, NULL,
        "page_count=131072",
        "eraseblock_pages=64",
        "block_count=2048",
        "marker_column=2048",
        "marker_length=1",
    };
    FILE *file;
    struct stat st;
    char line[192];
    unsigned int block, page, marker;
    char status[16];
    unsigned int i;

    if (!enabled) {
        if (map_path && *map_path) {
            error_setg(errp, "U-Boot marker map requires explicit opt-in");
            return false;
        }
        return true;
    }
    if (!map_path || !*map_path) {
        error_setg(errp, "U-Boot marker projection requires a DERIVED_FROM_RUN0005 map");
        return false;
    }
    if (stat(map_path, &st) < 0 || !S_ISREG(st.st_mode)) {
        error_setg_errno(errp, errno, "cannot stat U-Boot marker map '%s'", map_path);
        return false;
    }
    file = g_fopen(map_path, "rb");
    if (!file) {
        error_setg_errno(errp, errno, "cannot open U-Boot marker map '%s'", map_path);
        return false;
    }
    for (i = 0; i < G_N_ELEMENTS(headers); i++) {
        const char *prefix = NULL;

        if (!fgets(line, sizeof(line), file)) {
            error_setg(errp, "U-Boot marker map '%s' is truncated", map_path);
            goto fail;
        }
        line[strcspn(line, "\r\n")] = '\0';
        if (i == 2) {
            prefix = "samples_sha256=";
        } else if (i == 3) {
            prefix = "index_sha256=";
        } else if (i == 4) {
            prefix = "badblock_map_sha256=";
        }
        if (prefix) {
            if (!g_str_has_prefix(line, prefix) ||
                !macronix_marker_map_hex(line + strlen(prefix))) {
                error_setg(errp, "U-Boot marker map '%s' has invalid source identity", map_path);
                goto fail;
            }
        } else if (strcmp(line, headers[i])) {
            error_setg(errp, "U-Boot marker map '%s' has unsupported provenance or geometry", map_path);
            goto fail;
        }
    }
    for (i = 0; i < IMX6_MACRONIX_UBOOT_MARKER_BLOCK_COUNT; i++) {
        char canonical[48];

        if (!fgets(line, sizeof(line), file) ||
            sscanf(line, "%u\t%u\t%15s\t%x", &block, &page, status, &marker) != 4 ||
            block != i || page != i * 64 || marker > UINT8_MAX ||
            (strcmp(status, "GOOD") && strcmp(status, "BAD"))) {
            error_setg(errp, "U-Boot marker map '%s' has invalid block entry", map_path);
            goto fail;
        }
        g_snprintf(canonical, sizeof(canonical), "%u\t%u\t%s\t%02x",
                   block, page, status, marker);
        line[strcspn(line, "\r\n")] = '\0';
        if (strcmp(line, canonical)) {
            error_setg(errp, "U-Boot marker map '%s' has non-canonical block entry", map_path);
            goto fail;
        }
        view->block_health[i] = !strcmp(status, "BAD");
        view->marker[i] = marker;
    }
    if (fgets(line, sizeof(line), file)) {
        error_setg(errp, "U-Boot marker map '%s' has extra entries", map_path);
        goto fail;
    }
    fclose(file);
    view->enabled = true;
    macronix_uboot_marker_identity(view, &st);
    qemu_log_mask(LOG_GUEST_ERROR,
                  "XZ_NAND: diagnostic U-Boot marker projection attached "
                  "format=derived-linux-mtd-oob-view blocks=2048 path=%s\n", map_path);
    return true;

fail:
    fclose(file);
    imx6_macronix_nand_uboot_marker_close(view);
    return false;
}

bool imx6_macronix_nand_uboot_marker_matches(
    const IMX6MacronixNANDUBootMarkerView *view, uint64_t size,
    uint64_t device, uint64_t inode, int64_t mtime_sec, int64_t mtime_nsec)
{
    return view->enabled && view->map_size == size && view->map_device == device &&
           view->map_inode == inode && view->map_mtime_sec == mtime_sec &&
           view->map_mtime_nsec == mtime_nsec;
}

bool imx6_macronix_nand_uboot_marker_read(
    IMX6MacronixNANDState *nand, IMX6MacronixNANDUBootMarkerView *view,
    uint8_t *bytes, uint32_t length)
{
    uint32_t block;

    if (!view || !view->enabled || !bytes ||
        nand->phase != IMX6_MACRONIX_PAGE_READ_DATA ||
        !imx6_macronix_nand_ready(nand) || length != 1 ||
        nand->page_column_cursor != IMX6_MACRONIX_PAGE_MAIN_SIZE ||
        nand->page_number >= IMX6_MACRONIX_PAGE_COUNT || nand->page_number % 64) {
        nand->page_media_status = IMX6_MACRONIX_MEDIA_UBOOT_MARKER_REJECTED;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_NAND: uboot-marker-provenance-rejected page=%u column=%u bytes=%u opt-in=%u\n",
                      nand->page_number, nand->page_column_cursor, length,
                      view && view->enabled);
        return false;
    }
    block = nand->page_number / 64;
    bytes[0] = view->marker[block];
    nand->page_column_cursor++;
    nand->page_media_status = IMX6_MACRONIX_MEDIA_OK;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "XZ_NAND: uboot-marker-projection page=%u column=2048 block=%u "
                  "health=%s marker=0x%02x source=DERIVED_FROM_RUN0005\n",
                  nand->page_number, block,
                  view->block_health[block] ? "BAD" : "GOOD", bytes[0]);
    return true;
}

bool imx6_macronix_nand_command(IMX6MacronixNANDState *nand, uint8_t byte)
{
    qemu_log_mask(LOG_GUEST_ERROR, "XZ_NAND: command cs=0 opcode=0x%02x\n", byte);
    if (byte == 0x30) {
        if (nand->phase != IMX6_MACRONIX_PAGE_READ_PENDING) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "XZ_NAND: page-read-confirm-without-address\n");
            return false;
        }
        nand->phase = IMX6_MACRONIX_PAGE_READ_DATA;
        nand->cache_valid = true;
        nand->cache_pending = false;
        nand->page_column_cursor = nand->page_column;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_NAND: page-read confirmed page=%u column=%u area=%s\n",
                      nand->page_number, nand->page_column,
                      nand->page_column < IMX6_MACRONIX_PAGE_MAIN_SIZE ?
                      "main" : "oob");
        return true;
    }
    if (byte == 0x31) {
        if (nand->phase != IMX6_MACRONIX_PAGE_READ_DATA ||
            !nand->cache_valid || !nand->ready ||
            nand->page_number + 1 >= IMX6_MACRONIX_PAGE_COUNT) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "XZ_NAND: invalid-sequential-cache-read page=%u ready=%u valid=%u\n",
                          nand->page_number, nand->ready, nand->cache_valid);
            return false;
        }
        nand->phase = IMX6_MACRONIX_CACHE_SEQUENTIAL_ADDRESS;
        nand->cache_address_count = 0;
        nand->cache_next_page = nand->page_number + 1;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_NAND: sequential-cache-read command current-page=%u next-page=%u\n",
                      nand->page_number, nand->cache_next_page);
        return true;
    }
    if (byte == 0x3f) {
        /*
         * PM2129 final cache read: 3f replaces the final 31 after that row
         * has already been prefetched.  The extra two zero ALE bytes are an
         * observed XDJ-XZ/U-Boot helper convention, not a NAND address phase.
         */
        if (nand->phase != IMX6_MACRONIX_PAGE_READ_DATA ||
            !nand->cache_stream_active || !nand->cache_valid ||
            !nand->ready || nand->cache_pending || nand->cache_final_output ||
            nand->page_number != nand->cache_next_page) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "XZ_NAND: invalid-final-cache-read page=%u next-page=%u "
                          "ready=%u valid=%u stream=%u pending=%u final=%u\n",
                          nand->page_number, nand->cache_next_page,
                          nand->ready, nand->cache_valid,
                          nand->cache_stream_active, nand->cache_pending,
                          nand->cache_final_output);
            return false;
        }
        nand->phase = IMX6_MACRONIX_CACHE_FINAL_ADDRESS;
        nand->cache_address_count = 0;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_NAND: final-cache-read command output-page=%u\n",
                      nand->page_number);
        return true;
    }
    /* Program confirm is valid only while the setup/data phase is retained. */
    if (byte == 0x10) {
        if (nand->phase != IMX6_MACRONIX_PAGE_PROGRAM_DATA) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "XZ_NAND: page-program-confirm-without-data\n");
            return false;
        }
        nand->phase = IMX6_MACRONIX_IDLE;
        return true;
    }
    if (byte == 0xd0) {
        if (nand->phase != IMX6_MACRONIX_BLOCK_ERASE_CONFIRM) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "XZ_NAND: block-erase-confirm-without-address\n");
            return false;
        }
        nand->phase = IMX6_MACRONIX_IDLE;
        return true;
    }
    /* Any other valid command exits a prior READ-ID or parameter-page stream. */
    nand->phase = IMX6_MACRONIX_IDLE;
    nand->response_length = 0;
    nand->response_offset = 0;
    nand->parameter_page_offset = 0;
    nand->page_media_unavailable = false;
    nand->page_media_status = IMX6_MACRONIX_MEDIA_OK;
    if (byte == 0xff) {
        imx6_macronix_nand_reset(nand);
        qemu_log_mask(LOG_GUEST_ERROR, "XZ_NAND: reset ready=1\n");
        return true;
    }
    if (byte == 0x90) {
        nand->phase = IMX6_MACRONIX_READ_ID_ADDRESS;
        nand->response_length = 0;
        nand->response_offset = 0;
        return true;
    }
    if (byte == 0x70) {
        /*
         * Status has no address phase and remains selected for repeated
         * gpmi_read_byte() transfers until a later NAND command changes it.
         */
        nand->phase = IMX6_MACRONIX_STATUS_DATA;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_NAND: status selected ready=%u wp=%u fail=%u\n",
                      imx6_macronix_nand_ready(nand), nand->write_protected,
                      nand->last_operation_failed);
        return true;
    }
    if (byte == 0x80) {
        nand->phase = IMX6_MACRONIX_PAGE_PROGRAM_ADDRESS;
        nand->page_address_count = 0;
        return true;
    }
    if (byte == 0x60) {
        nand->phase = IMX6_MACRONIX_BLOCK_ERASE_ADDRESS;
        nand->page_address_count = 0;
        return true;
    }
    if (byte == 0xec) {
        nand->phase = IMX6_MACRONIX_PARAM_PAGE_ADDRESS;
        return true;
    }
    if (byte == 0x00) {
        nand->phase = IMX6_MACRONIX_PAGE_READ_ADDRESS;
        nand->page_address_count = 0;
        /* A new array-read setup supersedes any previously loaded cache. */
        nand->cache_address_count = 0;
        nand->cache_next_page = 0;
        nand->cache_valid = false;
        nand->cache_pending = false;
        nand->cache_stream_active = false;
        nand->cache_final_output = false;
        return true;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "XZ_NAND: unsupported-command opcode=0x%02x\n", byte);
    return false;
}

bool imx6_macronix_nand_address(IMX6MacronixNANDState *nand, uint8_t byte)
{
    qemu_log_mask(LOG_GUEST_ERROR, "XZ_NAND: address cs=0 byte=0x%02x\n", byte);
    if (nand->phase == IMX6_MACRONIX_PARAM_PAGE_ADDRESS) {
        uint8_t page[IMX6_MACRONIX_PARAM_PAGE_SIZE];

        if (byte != 0x00) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "XZ_NAND: unsupported-parameter-page-address byte=0x%02x\n",
                          byte);
            return false;
        }
        macronix_parameter_page(page);
        nand->parameter_page_offset = 0;
        nand->phase = IMX6_MACRONIX_PARAM_PAGE_DATA;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_NAND: parameter-page prepared copies=%u bytes=%u crc=0x%02x%02x\n",
                      IMX6_MACRONIX_PARAM_PAGE_COPIES,
                      IMX6_MACRONIX_PARAM_PAGE_TOTAL, page[255], page[254]);
        return true;
    }
    if (nand->phase == IMX6_MACRONIX_PAGE_READ_ADDRESS ||
        nand->phase == IMX6_MACRONIX_PAGE_PROGRAM_ADDRESS) {
        bool program = nand->phase == IMX6_MACRONIX_PAGE_PROGRAM_ADDRESS;

        if (nand->page_address_count >= ARRAY_SIZE(nand->page_address)) {
            qemu_log_mask(LOG_GUEST_ERROR, "XZ_NAND: page-address-too-many-addresses\n");
            return false;
        }
        nand->page_address[nand->page_address_count++] = byte;
        if (nand->page_address_count != ARRAY_SIZE(nand->page_address)) {
            return true;
        }
        nand->page_column = nand->page_address[0] |
                            (nand->page_address[1] << 8);
        nand->page_number = nand->page_address[2] |
                            (nand->page_address[3] << 8) |
                            (nand->page_address[4] << 16);
        if (nand->page_column >= IMX6_MACRONIX_PAGE_TOTAL ||
            nand->page_number >= IMX6_MACRONIX_PAGE_COUNT) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "XZ_NAND: page-address-out-of-range page=%u column=%u\n",
                          nand->page_number, nand->page_column);
            return false;
        }
        nand->phase = program ? IMX6_MACRONIX_PAGE_PROGRAM_DATA :
                                IMX6_MACRONIX_PAGE_READ_PENDING;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_NAND: page-%s address bytes=%02x:%02x:%02x:%02x:%02x page=%u block=%u page-in-block=%u column=%u area=%s\n",
                      program ? "program" : "read",
                      nand->page_address[0], nand->page_address[1],
                      nand->page_address[2], nand->page_address[3],
                      nand->page_address[4], nand->page_number,
                      nand->page_number / 64, nand->page_number % 64,
                      nand->page_column,
                      nand->page_column < IMX6_MACRONIX_PAGE_MAIN_SIZE ?
                      "main" : "oob");
        return true;
    }
    if (nand->phase == IMX6_MACRONIX_BLOCK_ERASE_ADDRESS) {
        if (nand->page_address_count >= 3) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "XZ_NAND: block-erase-too-many-addresses\n");
            return false;
        }
        nand->page_address[nand->page_address_count++] = byte;
        if (nand->page_address_count != 3) {
            return true;
        }
        nand->page_column = 0;
        nand->page_number = nand->page_address[0] |
                            (nand->page_address[1] << 8) |
                            (nand->page_address[2] << 16);
        if (nand->page_number >= IMX6_MACRONIX_PAGE_COUNT) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "XZ_NAND: block-erase-address-out-of-range page=%u\n",
                          nand->page_number);
            return false;
        }
        nand->phase = IMX6_MACRONIX_BLOCK_ERASE_CONFIRM;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_NAND: block-erase address bytes=%02x:%02x:%02x page=%u block=%u\n",
                      nand->page_address[0], nand->page_address[1],
                      nand->page_address[2], nand->page_number,
                      nand->page_number / 64);
        return true;
    }
    if (nand->phase == IMX6_MACRONIX_CACHE_SEQUENTIAL_ADDRESS) {
        if (nand->cache_address_count >= 2 || byte != 0x00) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "XZ_NAND: invalid-sequential-cache-column byte=0x%02x count=%u\n",
                          byte, nand->cache_address_count);
            return false;
        }
        nand->cache_address_count++;
        if (nand->cache_address_count != 2) {
            return true;
        }
        /*
         * The cache command starts prefetch of the next row, but its output
         * remains the current row until that output has been consumed.
         */
        nand->phase = IMX6_MACRONIX_PAGE_READ_DATA;
        nand->cache_valid = true;
        nand->cache_pending = true;
        nand->cache_stream_active = true;
        nand->cache_final_output = false;
        nand->ready = false;
        /* PM2129 rev. 1.4, table 14: tRCBSY read max is 25 us. */
        nand->ready_deadline_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 25000;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_NAND: sequential-cache-read pending page=%u column=0 ready-in-ns=25000\n",
                      nand->cache_next_page);
        return true;
    }
    if (nand->phase == IMX6_MACRONIX_CACHE_FINAL_ADDRESS) {
        if (nand->cache_address_count >= 2 || byte != 0x00) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "XZ_NAND: invalid-final-cache-helper byte=0x%02x count=%u\n",
                          byte, nand->cache_address_count);
            return false;
        }
        nand->cache_address_count++;
        if (nand->cache_address_count != 2) {
            return true;
        }
        nand->phase = IMX6_MACRONIX_PAGE_READ_DATA;
        nand->cache_pending = true;
        nand->cache_final_output = true;
        nand->ready = false;
        /* Model the documented cache-to-output ready bound deterministically. */
        nand->ready_deadline_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 25000;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_NAND: final-cache-read pending page=%u column=0 ready-in-ns=25000\n",
                      nand->page_number);
        return true;
    }
    if (nand->phase != IMX6_MACRONIX_READ_ID_ADDRESS) {
        qemu_log_mask(LOG_GUEST_ERROR, "XZ_NAND: invalid-address-phase\n");
        return false;
    }
    if (byte == 0x00) {
        static const uint8_t id[] = { 0xc2, 0xda, 0x90, 0x95, 0x06 };
        memcpy(nand->response, id, sizeof(id));
        nand->response_length = sizeof(id);
    } else if (byte == 0x20) {
        static const uint8_t onfi[] = { 'O', 'N', 'F', 'I' };
        memcpy(nand->response, onfi, sizeof(onfi));
        nand->response_length = sizeof(onfi);
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_NAND: unsupported-read-id-address byte=0x%02x\n", byte);
        return false;
    }
    nand->response_offset = 0;
    nand->phase = IMX6_MACRONIX_READ_ID_DATA;
    qemu_log_mask(LOG_GUEST_ERROR, "XZ_NAND: response prepared bytes=%u\n",
                  nand->response_length);
    return true;
}

bool imx6_macronix_nand_read(IMX6MacronixNANDState *nand, uint8_t *bytes,
                             uint32_t length, IMX6MacronixNANDMedia *media,
                             bool page_fixture_enabled)
{
    if (!bytes || !length) {
        qemu_log_mask(LOG_GUEST_ERROR, "XZ_NAND: invalid-read-buffer-or-length\n");
        return false;
    }
    nand->page_media_unavailable = false;
    nand->page_media_status = IMX6_MACRONIX_MEDIA_OK;
    if (nand->phase == IMX6_MACRONIX_STATUS_DATA) {
        if (length != 1) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "XZ_NAND: status-read-length-invalid bytes=%u\n", length);
            return false;
        }
        bool ready = imx6_macronix_nand_ready(nand);

        bytes[0] = macronix_status(nand, ready);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_NAND: status response byte=0x%02x ready=%u wp=%u fail=%u\n",
                      bytes[0], ready, nand->write_protected,
                      nand->last_operation_failed);
        return true;
    }
    if (nand->phase == IMX6_MACRONIX_PARAM_PAGE_DATA) {
        if (!macronix_parameter_page_read(nand, bytes, length)) {
            return false;
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_NAND: parameter-page response offset=%u bytes=%u first=0x%02x\n",
                      nand->parameter_page_offset, length, bytes[0]);
        nand->parameter_page_offset += length;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "XZ_NAND: response cs=0 bytes=%u first=0x%02x\n",
                      length, bytes[0]);
        return true;
    }
    if (nand->phase == IMX6_MACRONIX_PAGE_READ_DATA) {
        if (!imx6_macronix_nand_ready(nand)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "XZ_NAND: page-read-while-busy page=%u\n",
                          nand->page_number);
            return false;
        }
        if (nand->page_column_cursor > IMX6_MACRONIX_PAGE_TOTAL ||
            length > IMX6_MACRONIX_PAGE_TOTAL - nand->page_column_cursor) {
            nand->page_media_status = IMX6_MACRONIX_MEDIA_OUT_OF_RANGE;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "XZ_NAND: page-media-range-invalid page=%u column=%u bytes=%u\n",
                          nand->page_number, nand->page_column_cursor, length);
            return false;
        }
        if (media && media->file) {
            bool result = macronix_page_media_read(nand, media, bytes, length);

            if (result && nand->cache_final_output &&
                nand->page_column_cursor == IMX6_MACRONIX_PAGE_TOTAL) {
                nand->phase = IMX6_MACRONIX_IDLE;
                nand->cache_valid = false;
                nand->cache_pending = false;
                nand->cache_stream_active = false;
                nand->cache_final_output = false;
            }
            return result;
        }
        if (!page_fixture_enabled) {
            return macronix_page_media_read(nand, NULL, bytes, length);
        }
        {
            bool result = macronix_page_fixture_read(nand, bytes, length,
                                                     page_fixture_enabled);

            if (result && nand->cache_final_output &&
                nand->page_column_cursor == IMX6_MACRONIX_PAGE_TOTAL) {
                nand->phase = IMX6_MACRONIX_IDLE;
                nand->cache_valid = false;
                nand->cache_pending = false;
                nand->cache_stream_active = false;
                nand->cache_final_output = false;
            }
            return result;
        }
    }
    if (nand->phase != IMX6_MACRONIX_READ_ID_DATA ||
        length > nand->response_length - nand->response_offset) {
        return false;
    }
    memcpy(bytes, nand->response + nand->response_offset, length);
    nand->response_offset += length;
    qemu_log_mask(LOG_GUEST_ERROR, "XZ_NAND: response cs=0 bytes=%u first=0x%02x\n",
                  length, bytes[0]);
    return true;
}
