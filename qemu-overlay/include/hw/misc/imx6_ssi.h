/*
 * Minimal i.MX6 SSI register block.
 *
 * This provides the register contract used by Pioneer Linux 3.0.101's
 * imx-ssi driver.  It deliberately does not model serial pins, FIFO timing,
 * DMA requests, or host audio; SDMA descriptor ownership remains separate.
 */
#ifndef HW_MISC_IMX6_SSI_H
#define HW_MISC_IMX6_SSI_H

#include "hw/core/sysbus.h"
#include "qemu/audio.h"
#include "qemu/bswap.h"

#define TYPE_IMX6_SSI "imx6-ssi"
OBJECT_DECLARE_SIMPLE_TYPE(IMX6SSIState, IMX6_SSI)

#define IMX6_SSI_SIZE 0x1000
#define IMX6_SSI_AUDIO_FIFO_BYTES (44100 * 2 * 4)

/*
 * The sourced Pioneer ALSA streams are S24_LE: signed, LSB-aligned 24-bit
 * samples stored in four-byte little-endian slots.  QEMU's CoreAudio voice
 * here is AUDIO_FORMAT_S32, so simply forwarding the slot would make a
 * positive full-scale S24 sample about 48 dB quieter and would not guarantee
 * a correctly sign-extended negative value.  This is a host-sink conversion;
 * SSI registers and guest DMA memory retain their original bytes.
 */
static inline uint32_t imx6_ssi_s24le_to_s32le(uint32_t source)
{
    int32_t sample = source & 0x00ffffff;

    if (sample & 0x00800000) {
        sample |= ~0x00ffffff;
    }
    return (uint32_t)((int64_t)sample * 256);
}

static inline void imx6_ssi_convert_s24le_to_s32le(uint8_t *destination,
                                                    const uint8_t *source,
                                                    size_t bytes)
{
    size_t offset;

    for (offset = 0; offset + sizeof(uint32_t) <= bytes;
         offset += sizeof(uint32_t)) {
        stl_le_p(destination + offset,
                 imx6_ssi_s24le_to_s32le(ldl_le_p(source + offset)));
    }
}

/*
 * The optional CoreAudio sink must not wake just because ALSA is keeping a
 * cyclic standby stream alive.  For the XDJ source format, only the low
 * 24 bits are sample data; the upper byte is padding and is deliberately
 * ignored here.  This helper is host-sink policy only: it never changes the
 * SSI register state, guest DMA data, or SDMA completion timing.
 */
static inline bool imx6_ssi_pcm_has_signal(const uint8_t *source,
                                           size_t bytes, bool s24le)
{
    size_t offset;

    for (offset = 0; offset + sizeof(uint32_t) <= bytes;
         offset += sizeof(uint32_t)) {
        uint32_t sample = ldl_le_p(source + offset);

        if (s24le ? (sample & 0x00ffffff) : sample) {
            return true;
        }
    }
    return false;
}

struct IMX6SSIState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t regs[IMX6_SSI_SIZE / sizeof(uint32_t)];
    uint64_t dma_tx_bytes;
    uint64_t dma_rx_bytes;
    /* Explicitly opt-in host sink for already validated SDMA playback. */
    bool host_audio_enable;
    /* Source-observed XDJ PCM representation for the host sink only. */
    bool host_audio_s24le;
    /* Keep host writes out of the virtual SDMA timer when explicitly set. */
    bool host_audio_callback_only;
    /* Do not activate CoreAudio until the guest has queued real PCM. */
    bool host_audio_lazy_activate;
    /* Explicit runtime opt-in for aggregate audio health accounting. */
    bool host_audio_health_enable;
    bool host_audio_active;
    AudioBackend *audio_be;
    SWVoiceOut *audio_out;
    /* Host-side queue: SDMA must never discard a partial backend write. */
    GByteArray *audio_fifo;
    uint64_t host_audio_bytes;
    uint64_t host_audio_submitted_bytes;
    uint64_t host_audio_short_write_bytes;
    uint64_t host_audio_drop_bytes;
    uint64_t host_audio_callback_count;
    uint64_t host_audio_oldest_enqueue_host_ns;
    uint64_t host_audio_queue_max_age_ns;
    uint64_t host_audio_mix_min_hits;
    uint64_t host_audio_mix_live_zero_events;
    uint64_t host_audio_mix_source_frames;
    uint64_t host_audio_mix_min_frames;
    uint32_t host_audio_mix_active_voices;
    uint32_t host_audio_short_writes;
    uint32_t host_audio_fifo_peak_bytes;
};

/* SDMA's SSI-facing side of the source-observed cyclic PCM path. */
void imx6_ssi_dma_tx(IMX6SSIState *s, const uint8_t *data, size_t length);
void imx6_ssi_dma_rx(IMX6SSIState *s, uint8_t *data, size_t length);
/*
 * Pioneer imx-ssi starts a DMA stream only after its trigger callback sets
 * SSIEN plus the matching TE/RE and TDMAE/RDMAE bits.  SDMA must not complete
 * a cyclic period merely because its descriptors were prepared.
 */
bool imx6_ssi_dma_request_enabled(const IMX6SSIState *s, bool transmit);

#endif /* HW_MISC_IMX6_SSI_H */
