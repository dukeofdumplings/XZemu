/*
 * QEMU OS X CoreAudio audio driver
 *
 * Copyright (c) 2005 Mike Kronenberg
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include <CoreAudio/CoreAudio.h>
#include <AudioToolbox/AudioConverter.h>
#include <pthread.h>            /* pthread_X */

#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "qapi/visitor.h"
#include "qemu/audio.h"
#include "qom/object.h"
#include "audio_int.h"

#define TYPE_AUDIO_COREAUDIO "audio-coreaudio"
OBJECT_DECLARE_SIMPLE_TYPE(AudioCoreaudio, AUDIO_COREAUDIO)

typedef struct coreaudioVoiceOut CoreaudioVoiceOut;

struct AudioCoreaudio {
    AudioMixengBackend parent_obj;
    CoreaudioVoiceOut *out_voice;
    bool output_health_enable;
};

struct coreaudioVoiceOut {
    HWVoiceOut hw;
    pthread_mutex_t buf_mutex;
    AudioDeviceID device_id;
    int frame_size_setting;
    uint32_t buffer_count;
    UInt32 device_frame_size;
    AudioDeviceIOProcID ioprocid;
    AudioConverterRef converter;
    AudioStreamBasicDescription device_format;
    uint8_t *converter_input;
    size_t converter_input_size;
    bool enabled;
    uint32_t output_health_enable;
    /* Atomic, read-only IOProc health counters; the callback never logs. */
    uint64_t output_callbacks;
    uint64_t output_underruns;
    uint64_t output_missing_frames;
    uint64_t output_consumed_frames;
    uint64_t output_min_pending_frames;
    uint64_t output_max_pending_frames;
    uint64_t output_last_pending_frames;
    uint64_t output_device_frame_size;
    uint64_t output_ring_frames;
    uint64_t output_last_callback_host_ns;
    uint64_t output_max_callback_gap_ns;
    /*
     * Host-audio diagnosis only.  The emulation thread and CoreAudio's
     * real-time callback share buf_mutex.  Keep aggregate wait time so a
     * playback hitch can be attributed to this boundary without logging in
     * either hot path.
     */
    uint64_t output_vcpu_lock_wait_ns;
    uint64_t output_vcpu_max_lock_wait_ns;
    uint64_t output_ioproc_lock_wait_ns;
    uint64_t output_ioproc_max_lock_wait_ns;
};

static uint64_t coreaudio_host_monotonic_ns(void)
{
    struct timespec time;

    if (clock_gettime(CLOCK_MONOTONIC, &time)) {
        return 0;
    }
    return (uint64_t)time.tv_sec * NANOSECONDS_PER_SECOND + time.tv_nsec;
}

static void coreaudio_atomic_max(uint64_t *value, uint64_t candidate)
{
    uint64_t current = qatomic_read(value);

    while (candidate > current &&
           qatomic_cmpxchg(value, current, candidate) != current) {
        current = qatomic_read(value);
    }
}

static void coreaudio_atomic_min(uint64_t *value, uint64_t candidate)
{
    uint64_t current = qatomic_read(value);

    while (candidate < current &&
           qatomic_cmpxchg(value, current, candidate) != current) {
        current = qatomic_read(value);
    }
}

static void coreaudio_get_output_stat(Object *obj, Visitor *v,
                                      const char *name, void *opaque,
                                      Error **errp)
{
    AudioCoreaudio *s = AUDIO_COREAUDIO(obj);
    uint64_t value = 0;

    if (s->out_voice) {
        uintptr_t offset = (uintptr_t)opaque;
        uint64_t *stat = (uint64_t *)((uint8_t *)s->out_voice + offset);

        value = qatomic_read(stat);
        if ((uintptr_t)opaque == offsetof(CoreaudioVoiceOut,
                                          output_min_pending_frames) &&
            value == UINT64_MAX) {
            value = 0;
        }
    }
    visit_type_uint64(v, name, &value, errp);
}

static void coreaudio_add_output_stat(Object *obj, const char *name,
                                      size_t offset)
{
    object_property_add(obj, name, "uint64", coreaudio_get_output_stat,
                        NULL, NULL, (void *)(uintptr_t)offset);
}

static bool coreaudio_get_output_health_enable(Object *obj, Error **errp)
{
    return AUDIO_COREAUDIO(obj)->output_health_enable;
}

static void coreaudio_set_output_health_enable(Object *obj, bool value,
                                                Error **errp)
{
    AudioCoreaudio *s = AUDIO_COREAUDIO(obj);

    s->output_health_enable = value;
    if (s->out_voice) {
        qatomic_set(&s->out_voice->output_health_enable, value);
    }
}

static void audio_coreaudio_init(Object *obj)
{
    object_property_add_bool(obj, "output-health-enable",
                             coreaudio_get_output_health_enable,
                             coreaudio_set_output_health_enable);
    coreaudio_add_output_stat(obj, "output-callbacks",
                              offsetof(CoreaudioVoiceOut, output_callbacks));
    coreaudio_add_output_stat(obj, "output-underruns",
                              offsetof(CoreaudioVoiceOut, output_underruns));
    coreaudio_add_output_stat(obj, "output-missing-frames",
                              offsetof(CoreaudioVoiceOut, output_missing_frames));
    coreaudio_add_output_stat(obj, "output-consumed-frames",
                              offsetof(CoreaudioVoiceOut, output_consumed_frames));
    coreaudio_add_output_stat(obj, "output-min-pending-frames",
                              offsetof(CoreaudioVoiceOut, output_min_pending_frames));
    coreaudio_add_output_stat(obj, "output-max-pending-frames",
                              offsetof(CoreaudioVoiceOut, output_max_pending_frames));
    coreaudio_add_output_stat(obj, "output-last-pending-frames",
                              offsetof(CoreaudioVoiceOut, output_last_pending_frames));
    coreaudio_add_output_stat(obj, "output-device-frame-size",
                              offsetof(CoreaudioVoiceOut, output_device_frame_size));
    coreaudio_add_output_stat(obj, "output-ring-frames",
                              offsetof(CoreaudioVoiceOut, output_ring_frames));
    coreaudio_add_output_stat(obj, "output-last-callback-host-ns",
                              offsetof(CoreaudioVoiceOut, output_last_callback_host_ns));
    coreaudio_add_output_stat(obj, "output-max-callback-gap-ns",
                              offsetof(CoreaudioVoiceOut, output_max_callback_gap_ns));
    coreaudio_add_output_stat(obj, "output-vcpu-lock-wait-ns",
                              offsetof(CoreaudioVoiceOut, output_vcpu_lock_wait_ns));
    coreaudio_add_output_stat(obj, "output-vcpu-max-lock-wait-ns",
                              offsetof(CoreaudioVoiceOut,
                                       output_vcpu_max_lock_wait_ns));
    coreaudio_add_output_stat(obj, "output-ioproc-lock-wait-ns",
                              offsetof(CoreaudioVoiceOut, output_ioproc_lock_wait_ns));
    coreaudio_add_output_stat(obj, "output-ioproc-max-lock-wait-ns",
                              offsetof(CoreaudioVoiceOut,
                                       output_ioproc_max_lock_wait_ns));
}


static const AudioObjectPropertyAddress voice_out_addr = {
    kAudioHardwarePropertyDefaultOutputDevice,
    kAudioObjectPropertyScopeGlobal,
    kAudioObjectPropertyElementMain
};

static OSStatus coreaudio_get_voice_out(AudioDeviceID *id)
{
    UInt32 size = sizeof(*id);

    return AudioObjectGetPropertyData(kAudioObjectSystemObject,
                                      &voice_out_addr,
                                      0,
                                      NULL,
                                      &size,
                                      id);
}

static OSStatus coreaudio_get_out_framesizerange(AudioDeviceID id,
                                                 AudioValueRange *framerange)
{
    UInt32 size = sizeof(*framerange);
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyBufferFrameSizeRange,
        kAudioDevicePropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };

    return AudioObjectGetPropertyData(id,
                                      &addr,
                                      0,
                                      NULL,
                                      &size,
                                      framerange);
}

static OSStatus coreaudio_get_out_framesize(AudioDeviceID id, UInt32 *framesize)
{
    UInt32 size = sizeof(*framesize);
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyBufferFrameSize,
        kAudioDevicePropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };

    return AudioObjectGetPropertyData(id,
                                      &addr,
                                      0,
                                      NULL,
                                      &size,
                                      framesize);
}

static OSStatus coreaudio_get_out_streamformat(AudioDeviceID id,
                                               AudioStreamBasicDescription *d)
{
    UInt32 size = sizeof(*d);
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyStreamFormat,
        kAudioDevicePropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };

    return AudioObjectGetPropertyData(id, &addr, 0, NULL, &size, d);
}

static OSStatus coreaudio_get_out_isrunning(AudioDeviceID id, UInt32 *result)
{
    UInt32 size = sizeof(*result);
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyDeviceIsRunning,
        kAudioDevicePropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };

    return AudioObjectGetPropertyData(id,
                                      &addr,
                                      0,
                                      NULL,
                                      &size,
                                      result);
}

static void coreaudio_logstatus(OSStatus status)
{
    const char *str = "BUG";

    switch (status) {
    case kAudioHardwareNoError:
        str = "kAudioHardwareNoError";
        break;

    case kAudioHardwareNotRunningError:
        str = "kAudioHardwareNotRunningError";
        break;

    case kAudioHardwareUnspecifiedError:
        str = "kAudioHardwareUnspecifiedError";
        break;

    case kAudioHardwareUnknownPropertyError:
        str = "kAudioHardwareUnknownPropertyError";
        break;

    case kAudioHardwareBadPropertySizeError:
        str = "kAudioHardwareBadPropertySizeError";
        break;

    case kAudioHardwareIllegalOperationError:
        str = "kAudioHardwareIllegalOperationError";
        break;

    case kAudioHardwareBadDeviceError:
        str = "kAudioHardwareBadDeviceError";
        break;

    case kAudioHardwareBadStreamError:
        str = "kAudioHardwareBadStreamError";
        break;

    case kAudioHardwareUnsupportedOperationError:
        str = "kAudioHardwareUnsupportedOperationError";
        break;

    case kAudioDeviceUnsupportedFormatError:
        str = "kAudioDeviceUnsupportedFormatError";
        break;

    case kAudioDevicePermissionsError:
        str = "kAudioDevicePermissionsError";
        break;

    default:
        error_printf(" Reason: status code %" PRId32, (int32_t)status);
        return;
    }

    error_printf(" Reason: %s", str);
}

static void G_GNUC_PRINTF(2, 3) coreaudio_logerr(OSStatus status,
                                                const char *fmt, ...)
{
    va_list ap;

    error_printf("coreaudio: ");
    va_start(ap, fmt);
    error_vprintf(fmt, ap);
    va_end(ap);
    coreaudio_logstatus(status);
    error_printf("\n");
}

static void G_GNUC_PRINTF(3, 4) coreaudio_logerr2(OSStatus status,
                                                  const char *typ,
                                                  const char *fmt, ...)
{
    va_list ap;

    error_printf("coreaudio: Could not initialize %s: ", typ);
    va_start(ap, fmt);
    error_vprintf(fmt, ap);
    va_end(ap);
    coreaudio_logstatus(status);
    error_printf("\n");
}

#define coreaudio_playback_logerr(status, ...) \
    coreaudio_logerr2(status, "playback", __VA_ARGS__)

static int coreaudio_voice_out_buf_lock(CoreaudioVoiceOut *core,
                                        const char *fn_name,
                                        bool ioproc)
{
    int err;
    bool health_enable = qatomic_read(&core->output_health_enable);
    uint64_t before = health_enable ? coreaudio_host_monotonic_ns() : 0;

    err = pthread_mutex_lock(&core->buf_mutex);
    if (health_enable && before) {
        uint64_t after = coreaudio_host_monotonic_ns();

        if (after > before) {
            uint64_t waited = after - before;

            if (ioproc) {
                qatomic_fetch_add(&core->output_ioproc_lock_wait_ns, waited);
                coreaudio_atomic_max(&core->output_ioproc_max_lock_wait_ns,
                                     waited);
            } else {
                qatomic_fetch_add(&core->output_vcpu_lock_wait_ns, waited);
                coreaudio_atomic_max(&core->output_vcpu_max_lock_wait_ns,
                                     waited);
            }
        }
    }
    if (err) {
        error_report("coreaudio: Could not lock voice for %s: %s",
                     fn_name, strerror(err));
        return -1;
    }
    return 0;
}

static int coreaudio_voice_out_buf_unlock(CoreaudioVoiceOut *core,
                                          const char *fn_name)
{
    int err;

    err = pthread_mutex_unlock(&core->buf_mutex);
    if (err) {
        error_report("coreaudio: Could not unlock voice for %s: %s",
                     fn_name, strerror(err));
        return -1;
    }
    return 0;
}

#define COREAUDIO_WRAPPER_FUNC(name, ret_type, args_decl, args)       \
    static ret_type glue(coreaudio_, name)args_decl                   \
    {                                                                 \
        CoreaudioVoiceOut *core = (CoreaudioVoiceOut *)hw;            \
        ret_type ret;                                                 \
                                                                      \
        if (coreaudio_voice_out_buf_lock(core, "coreaudio_" #name,     \
                                         false)) {                       \
            return 0;                                                 \
        }                                                             \
                                                                      \
        ret = glue(audio_generic_, name)args;                         \
                                                                      \
        coreaudio_voice_out_buf_unlock(core, "coreaudio_" #name);     \
        return ret;                                                   \
    }
COREAUDIO_WRAPPER_FUNC(buffer_get_free, size_t, (HWVoiceOut *hw), (hw))
COREAUDIO_WRAPPER_FUNC(get_buffer_out, void *, (HWVoiceOut *hw, size_t *size),
                       (hw, size))
COREAUDIO_WRAPPER_FUNC(put_buffer_out, size_t,
                       (HWVoiceOut *hw, void *buf, size_t size),
                       (hw, buf, size))
COREAUDIO_WRAPPER_FUNC(write, size_t, (HWVoiceOut *hw, void *buf, size_t size),
                       (hw, buf, size))
#undef COREAUDIO_WRAPPER_FUNC

static OSStatus coreaudio_converter_input(AudioConverterRef converter,
                                          UInt32 *packets,
                                          AudioBufferList *data,
                                          AudioStreamPacketDescription **desc,
                                          void *opaque)
{
    CoreaudioVoiceOut *core = opaque;
    HWVoiceOut *hw = &core->hw;
    size_t wanted = (size_t)*packets * hw->info.bytes_per_frame;
    size_t available = MIN(wanted, hw->pending_emul);
    size_t copied = 0;

    (void)converter;
    (void)desc;
    while (copied < available) {
        size_t start = audio_ring_posb(hw->pos_emul, hw->pending_emul,
                                       hw->size_emul);
        size_t chunk = MIN(available - copied, hw->size_emul - start);

        memcpy(core->converter_input + copied, hw->buf_emul + start, chunk);
        hw->pending_emul -= chunk;
        copied += chunk;
    }
    *packets = copied / hw->info.bytes_per_frame;
    data->mNumberBuffers = 1;
    data->mBuffers[0].mNumberChannels = hw->info.nchannels;
    data->mBuffers[0].mDataByteSize = copied;
    data->mBuffers[0].mData = core->converter_input;
    return noErr;
}

/*
 * callback to feed audiooutput buffer. called without BQL.
 * allowed to lock "buf_mutex", but disallowed to have any other locks.
 */
static OSStatus out_device_ioproc(
    AudioDeviceID inDevice,
    const AudioTimeStamp *inNow,
    const AudioBufferList *inInputData,
    const AudioTimeStamp *inInputTime,
    AudioBufferList *outOutputData,
    const AudioTimeStamp *inOutputTime,
    void *hwptr)
{
    UInt32 frame_size, pending_frames;
    void *out = outOutputData->mBuffers[0].mData;
    HWVoiceOut *hw = hwptr;
    CoreaudioVoiceOut *core = hwptr;
    size_t len;
    bool health_enable = qatomic_read(&core->output_health_enable);
    uint64_t now_host_ns = 0;

    if (health_enable) {
        uint64_t previous_host_ns;

        now_host_ns = coreaudio_host_monotonic_ns();
        previous_host_ns = qatomic_read(&core->output_last_callback_host_ns);
        if (now_host_ns && previous_host_ns && now_host_ns > previous_host_ns) {
            coreaudio_atomic_max(&core->output_max_callback_gap_ns,
                                 now_host_ns - previous_host_ns);
        }
        if (now_host_ns) {
            qatomic_set(&core->output_last_callback_host_ns, now_host_ns);
        }
    }

    if (coreaudio_voice_out_buf_lock(core, "out_device_ioproc", true)) {
        inInputTime = 0;
        return 0;
    }

    if (inDevice != core->device_id) {
        coreaudio_voice_out_buf_unlock(core, "out_device_ioproc(old device)");
        return 0;
    }

    if (core->converter) {
        UInt32 output_packets = outOutputData->mBuffers[0].mDataByteSize /
                                core->device_format.mBytesPerPacket;
        UInt32 produced;
        OSStatus status;

        status = AudioConverterFillComplexBuffer(core->converter,
                                                 coreaudio_converter_input,
                                                 core, &output_packets,
                                                 outOutputData, NULL);
        produced = output_packets * core->device_format.mBytesPerPacket;
        if (status != noErr) {
            output_packets = 0;
            produced = 0;
        }
        if (produced < outOutputData->mBuffers[0].mDataByteSize) {
            memset((uint8_t *)outOutputData->mBuffers[0].mData + produced, 0,
                   outOutputData->mBuffers[0].mDataByteSize - produced);
        }
        coreaudio_voice_out_buf_unlock(core, "out_device_ioproc(converter)");
        return noErr;
    }

    frame_size = core->device_frame_size;
    pending_frames = hw->pending_emul / hw->info.bytes_per_frame;
    if (health_enable) {
        qatomic_fetch_add(&core->output_callbacks, 1);
        qatomic_set(&core->output_last_pending_frames, pending_frames);
        coreaudio_atomic_min(&core->output_min_pending_frames, pending_frames);
        coreaudio_atomic_max(&core->output_max_pending_frames, pending_frames);
    }

    /*
     * Keep the host presentation clock moving across a short callback.  The
     * old all-or-nothing path retained partial PCM for the next callback,
     * turning a small host scheduling miss into an audible full-period
     * dropout.  This is host-only: SDMA descriptors, SSI state, and guest
     * period IRQ cadence remain untouched.
     */
    if (pending_frames < frame_size && health_enable) {
        qatomic_fetch_add(&core->output_underruns, 1);
        qatomic_fetch_add(&core->output_missing_frames,
                          frame_size - pending_frames);
    }

    if (health_enable) {
        qatomic_fetch_add(&core->output_consumed_frames,
                          MIN(pending_frames, frame_size));
    }

    len = MIN(pending_frames, frame_size) * hw->info.bytes_per_frame;
    while (len) {
        size_t write_len, start;

        start = audio_ring_posb(hw->pos_emul, hw->pending_emul, hw->size_emul);
        assert(start < hw->size_emul);

        write_len = MIN(MIN(hw->pending_emul, len),
                        hw->size_emul - start);

        memcpy(out, hw->buf_emul + start, write_len);
        hw->pending_emul -= write_len;
        len -= write_len;
        out += write_len;
    }
    if (pending_frames < frame_size) {
        memset(out, 0, (frame_size - pending_frames) *
               hw->info.bytes_per_frame);
    }

    coreaudio_voice_out_buf_unlock(core, "out_device_ioproc");
    return 0;
}

static OSStatus init_out_device(CoreaudioVoiceOut *core)
{
    AudioDeviceID device_id;
    AudioDeviceIOProcID ioprocid;
    AudioValueRange value_range;
    OSStatus status;
    UInt32 device_frame_size;

    AudioStreamBasicDescription stream_basic_description = {
        .mBitsPerChannel = audio_format_bits(core->hw.info.af),
        .mBytesPerFrame = core->hw.info.bytes_per_frame,
        .mBytesPerPacket = core->hw.info.bytes_per_frame,
        .mChannelsPerFrame = core->hw.info.nchannels,
        .mFormatFlags = kLinearPCMFormatFlagIsFloat,
        .mFormatID = kAudioFormatLinearPCM,
        .mFramesPerPacket = 1,
        .mSampleRate = core->hw.info.freq
    };

    status = coreaudio_get_voice_out(&device_id);
    if (status != kAudioHardwareNoError) {
        coreaudio_playback_logerr(status,
                                  "Could not get default output device");
        return status;
    }
    if (device_id == kAudioDeviceUnknown) {
        error_report("coreaudio: Could not initialize playback: "
                     "Unknown audio device");
        return status;
    }

    /* get minimum and maximum buffer frame sizes */
    status = coreaudio_get_out_framesizerange(device_id, &value_range);
    if (status == kAudioHardwareBadObjectError) {
        return 0;
    }
    if (status != kAudioHardwareNoError) {
        coreaudio_playback_logerr(status,
                                  "Could not get device buffer frame range");
        return status;
    }

    if (value_range.mMinimum > core->frame_size_setting) {
        device_frame_size = value_range.mMinimum;
        warn_report("coreaudio: Upsizing buffer frames to %f",
                    value_range.mMinimum);
    } else if (value_range.mMaximum < core->frame_size_setting) {
        device_frame_size = value_range.mMaximum;
        warn_report("coreaudio: Downsizing buffer frames to %f",
                    value_range.mMaximum);
    } else {
        device_frame_size = core->frame_size_setting;
    }

    /* get Buffer Frame Size */
    status = coreaudio_get_out_framesize(device_id, &device_frame_size);
    if (status == kAudioHardwareBadObjectError) {
        return 0;
    }
    if (status != kAudioHardwareNoError) {
        coreaudio_playback_logerr(status,
                                  "Could not get device buffer frame size");
        return status;
    }

    {
        AudioStreamBasicDescription actual = { 0 };

        status = coreaudio_get_out_streamformat(device_id, &actual);
        if (status != kAudioHardwareNoError) {
            coreaudio_playback_logerr(status,
                                      "Could not read negotiated stream format");
            return status;
        }
        core->device_format = actual;
        if (memcmp(&actual, &stream_basic_description, sizeof(actual))) {
            status = AudioConverterNew(&stream_basic_description, &actual,
                                       &core->converter);
            if (status != noErr) {
                coreaudio_playback_logerr(status,
                                          "Could not create output converter");
                return status;
            }
            core->converter_input_size = MAX((size_t)65536,
                                             (size_t)core->frame_size_setting *
                                             core->hw.info.bytes_per_frame * 8);
            core->converter_input = g_malloc(core->converter_input_size);
        }
    }

    /*
     * set Callback.
     *
     * On macOS 11.3.1, Core Audio calls AudioDeviceIOProc after calling an
     * internal function named HALB_Mutex::Lock(), which locks a mutex in
     * HALB_IOThread::Entry(void*). HALB_Mutex::Lock() is also called in
     * AudioObjectGetPropertyData, which is called by coreaudio driver.
     * Therefore, the specified callback must be designed to avoid a deadlock
     * with the callers of AudioObjectGetPropertyData.
     */
    ioprocid = NULL;
    status = AudioDeviceCreateIOProcID(device_id,
                                       out_device_ioproc,
                                       &core->hw,
                                       &ioprocid);
    if (status == kAudioHardwareBadDeviceError) {
        return 0;
    }
    if (status != kAudioHardwareNoError || ioprocid == NULL) {
        coreaudio_playback_logerr(status, "Could not set IOProc");
        return status;
    }

    core->device_id = device_id;
    core->device_frame_size = device_frame_size;
    core->hw.samples = core->buffer_count * core->device_frame_size;
    qatomic_set(&core->output_device_frame_size, core->device_frame_size);
    qatomic_set(&core->output_ring_frames, core->hw.samples);
    audio_generic_initialize_buffer_out(&core->hw);
    core->ioprocid = ioprocid;

    return 0;
}

static void fini_out_device(CoreaudioVoiceOut *core)
{
    OSStatus status;
    UInt32 isrunning;

    /* stop playback */
    status = coreaudio_get_out_isrunning(core->device_id, &isrunning);
    if (status != kAudioHardwareBadObjectError) {
        if (status != kAudioHardwareNoError) {
            coreaudio_logerr(status,
                             "Could not determine whether device is playing");
        }

        if (isrunning) {
            status = AudioDeviceStop(core->device_id, core->ioprocid);
            if (status != kAudioHardwareBadDeviceError && status != kAudioHardwareNoError) {
                coreaudio_logerr(status, "Could not stop playback");
            }
        }
    }

    /* remove callback */
    status = AudioDeviceDestroyIOProcID(core->device_id,
                                        core->ioprocid);
    if (status != kAudioHardwareBadDeviceError && status != kAudioHardwareNoError) {
        coreaudio_logerr(status, "Could not remove IOProc");
    }
    core->device_id = kAudioDeviceUnknown;
    if (core->converter) {
        AudioConverterDispose(core->converter);
        core->converter = NULL;
    }
    g_clear_pointer(&core->converter_input, g_free);
    core->converter_input_size = 0;
}

static void update_out_device_playback_state(CoreaudioVoiceOut *core)
{
    OSStatus status;
    UInt32 isrunning;

    status = coreaudio_get_out_isrunning(core->device_id, &isrunning);
    if (status != kAudioHardwareNoError) {
        if (status != kAudioHardwareBadObjectError) {
            coreaudio_logerr(status,
                             "Could not determine whether device is playing");
        }

        return;
    }

    if (core->enabled) {
        /* start playback */
        if (!isrunning) {
            status = AudioDeviceStart(core->device_id, core->ioprocid);
            if (status != kAudioHardwareBadDeviceError && status != kAudioHardwareNoError) {
                coreaudio_logerr(status, "Could not resume playback");
            }
        }
    } else {
        /* stop playback */
        if (isrunning) {
            status = AudioDeviceStop(core->device_id,
                                     core->ioprocid);
            if (status != kAudioHardwareBadDeviceError && status != kAudioHardwareNoError) {
                coreaudio_logerr(status, "Could not pause playback");
            }
        }
    }
}

/* called without BQL. */
static OSStatus handle_voice_out_change(
    AudioObjectID in_object_id,
    UInt32 in_number_addresses,
    const AudioObjectPropertyAddress *in_addresses,
    void *in_client_data)
{
    CoreaudioVoiceOut *core = in_client_data;

    bql_lock();

    if (core->device_id) {
        fini_out_device(core);
    }

    init_out_device(core);

    if (core->device_id) {
        update_out_device_playback_state(core);
    }

    bql_unlock();
    return 0;
}

static int coreaudio_init_out(HWVoiceOut *hw, struct audsettings *as)
{
    OSStatus status;
    CoreaudioVoiceOut *core = (CoreaudioVoiceOut *)hw;
    int err;
    Audiodev *dev = hw->s->dev;
    AudiodevCoreaudioPerDirectionOptions *cpdo = dev->u.coreaudio.out;
    struct audsettings obt_as;

    /* create mutex */
    err = pthread_mutex_init(&core->buf_mutex, NULL);
    if (err) {
        error_report("coreaudio: Could not create mutex: %s", strerror(err));
        return -1;
    }

    obt_as = *as;
    as = &obt_as;
    as->fmt = AUDIO_FORMAT_F32;
    audio_pcm_init_info(&hw->info, as);

    core->frame_size_setting = audio_buffer_frames(
        qapi_AudiodevCoreaudioPerDirectionOptions_base(cpdo), as, 11610);

    core->buffer_count = cpdo->has_buffer_count ? cpdo->buffer_count : 4;
    qatomic_set(&core->output_callbacks, 0);
    qatomic_set(&core->output_underruns, 0);
    qatomic_set(&core->output_missing_frames, 0);
    qatomic_set(&core->output_consumed_frames, 0);
    qatomic_set(&core->output_min_pending_frames, UINT64_MAX);
    qatomic_set(&core->output_max_pending_frames, 0);
    qatomic_set(&core->output_last_pending_frames, 0);
    qatomic_set(&core->output_device_frame_size, 0);
    qatomic_set(&core->output_ring_frames, 0);
    qatomic_set(&core->output_last_callback_host_ns, 0);
    qatomic_set(&core->output_max_callback_gap_ns, 0);
    qatomic_set(&core->output_health_enable,
                AUDIO_COREAUDIO(hw->s)->output_health_enable);
    AUDIO_COREAUDIO(hw->s)->out_voice = core;

    status = AudioObjectAddPropertyListener(kAudioObjectSystemObject,
                                            &voice_out_addr,
                                            handle_voice_out_change,
                                            core);
    if (status != kAudioHardwareNoError) {
        coreaudio_playback_logerr(status,
                                  "Could not listen to voice property change");
        return -1;
    }

    if (init_out_device(core)) {
        status = AudioObjectRemovePropertyListener(kAudioObjectSystemObject,
                                                   &voice_out_addr,
                                                   handle_voice_out_change,
                                                   core);
        if (status != kAudioHardwareNoError) {
            coreaudio_playback_logerr(status,
                                      "Could not remove voice property change listener");
        }

        return -1;
    }

    return 0;
}

static void coreaudio_fini_out (HWVoiceOut *hw)
{
    OSStatus status;
    int err;
    CoreaudioVoiceOut *core = (CoreaudioVoiceOut *)hw;

    if (AUDIO_COREAUDIO(hw->s)->out_voice == core) {
        AUDIO_COREAUDIO(hw->s)->out_voice = NULL;
    }

    status = AudioObjectRemovePropertyListener(kAudioObjectSystemObject,
                                               &voice_out_addr,
                                               handle_voice_out_change,
                                               core);
    if (status != kAudioHardwareNoError) {
        coreaudio_logerr(status, "Could not remove voice property change listener");
    }

    fini_out_device(core);

    /* destroy mutex */
    err = pthread_mutex_destroy(&core->buf_mutex);
    if (err) {
        error_report("coreaudio: Could not destroy mutex: %s", strerror(err));
    }
}

static void coreaudio_enable_out(HWVoiceOut *hw, bool enable)
{
    CoreaudioVoiceOut *core = (CoreaudioVoiceOut *)hw;

    core->enabled = enable;
    update_out_device_playback_state(core);
}

static void audio_coreaudio_class_init(ObjectClass *klass, const void *data)
{
    AudioMixengBackendClass *k = AUDIO_MIXENG_BACKEND_CLASS(klass);

    k->max_voices_out = 1;
    k->max_voices_in = 0;
    k->voice_size_out = sizeof(CoreaudioVoiceOut);
    k->voice_size_in = 0;

    k->init_out = coreaudio_init_out;
    k->fini_out = coreaudio_fini_out;
    /* wrapper for audio_generic_write */
    k->write = coreaudio_write;
    /* wrapper for audio_generic_buffer_get_free */
    k->buffer_get_free = coreaudio_buffer_get_free;
    /* wrapper for audio_generic_get_buffer_out */
    k->get_buffer_out = coreaudio_get_buffer_out;
    /* wrapper for audio_generic_put_buffer_out */
    k->put_buffer_out = coreaudio_put_buffer_out;
    k->enable_out = coreaudio_enable_out;
}

static const TypeInfo audio_types[] = {
    {
        .name = TYPE_AUDIO_COREAUDIO,
        .parent = TYPE_AUDIO_MIXENG_BACKEND,
        .instance_size = sizeof(AudioCoreaudio),
        .instance_init = audio_coreaudio_init,
        .class_init = audio_coreaudio_class_init,
    },
};

DEFINE_TYPES(audio_types)
module_obj(TYPE_AUDIO_COREAUDIO);
