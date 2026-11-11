/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "SDL_internal.h"

#ifdef SDL_AUDIO_DRIVER_ASIO

#include "../../core/windows/SDL_windows.h"
#include <objbase.h>
#include "SDL_asio.h"

// ASIO drivers are full duplex and only one can be loaded at a time. The driver callbacks
// have no user data, so all of the state for the loaded driver is global.
//
// Each driver shows up as a playback device and a recording device. When both are opened,
// they share a single set of driver buffers and are serviced by the same bufferSwitch.
//
// The playback and recording device pointers are only changed while the driver is stopped,
// so the driver thread can read them without locking.
static struct
{
    SDL_Mutex *lock;

    ASIO_DriverHandle **handles;
    int num_handles;

    ASIO_DriverHandle *loaded_handle;
    IASIO *iasio;
    bool com_initialized;
    long num_inputs;
    long num_outputs;
    long buffer_frames;
    double sample_rate;
    bool post_output;
    bool buffers_created;
    bool running;

    SDL_AudioDevice *playback;
    SDL_AudioDevice *recording;
    int playback_channels;
    int recording_channels;

    // Playback channels first, then recording channels.
    ASIOBufferInfo buffer_infos[SDL_MAX_CHANNELMAP_CHANNELS * 2];
    ASIOSampleType sample_types[SDL_MAX_CHANNELMAP_CHANNELS * 2];
    ASIOCallbacks callbacks;

    long buffer_index;
    bool wrote_output;
    SDL_AtomicInt overloads;
} asio;

static double ASIO_Clamp(float sample)
{
    if (sample < -1.0f) {
        return -1.0;
    } else if (sample > 1.0f) {
        return 1.0;
    }
    return sample;
}

static Sint32 ASIO_FloatToInt(float sample, int bits)
{
    const double scale = (double)(1u << (bits - 1));
    double value = ASIO_Clamp(sample) * scale;
    if (value > scale - 1.0) {
        value = scale - 1.0;
    }
    return (Sint32)value;
}

static float ASIO_IntToFloat(Sint32 sample, int bits)
{
    return (float)((double)sample / (double)(1u << (bits - 1)));
}

static int ASIO_GetSampleBits(ASIOSampleType type)
{
    switch (type) {
    case ASIOSTInt16LSB:
        return 16;
    case ASIOSTInt24LSB:
        return 24;
    case ASIOSTInt32LSB:
        return 32;
    case ASIOSTInt32LSB16:
        return 16;
    case ASIOSTInt32LSB18:
        return 18;
    case ASIOSTInt32LSB20:
        return 20;
    case ASIOSTInt32LSB24:
        return 24;
    case ASIOSTFloat32LSB:
        return 32;
    case ASIOSTFloat64LSB:
        return 64;
    default:
        return 0;
    }
}

static void ASIO_WriteChannel(void *buffer, ASIOSampleType type, const float *src, int stride, int frames)
{
    const int bits = ASIO_GetSampleBits(type);
    int i;

    switch (type) {
    case ASIOSTInt16LSB:
    {
        Sint16 *dst = (Sint16 *)buffer;
        for (i = 0; i < frames; ++i, src += stride) {
            dst[i] = (Sint16)ASIO_FloatToInt(*src, bits);
        }
        break;
    }
    case ASIOSTInt24LSB:
    {
        Uint8 *dst = (Uint8 *)buffer;
        for (i = 0; i < frames; ++i, src += stride, dst += 3) {
            const Sint32 value = ASIO_FloatToInt(*src, bits);
            dst[0] = (Uint8)(value & 0xFF);
            dst[1] = (Uint8)((value >> 8) & 0xFF);
            dst[2] = (Uint8)((value >> 16) & 0xFF);
        }
        break;
    }
    case ASIOSTInt32LSB:
    case ASIOSTInt32LSB16:
    case ASIOSTInt32LSB18:
    case ASIOSTInt32LSB20:
    case ASIOSTInt32LSB24:
    {
        Sint32 *dst = (Sint32 *)buffer;
        for (i = 0; i < frames; ++i, src += stride) {
            dst[i] = ASIO_FloatToInt(*src, bits);
        }
        break;
    }
    case ASIOSTFloat32LSB:
    {
        float *dst = (float *)buffer;
        for (i = 0; i < frames; ++i, src += stride) {
            dst[i] = *src;
        }
        break;
    }
    case ASIOSTFloat64LSB:
    {
        double *dst = (double *)buffer;
        for (i = 0; i < frames; ++i, src += stride) {
            dst[i] = *src;
        }
        break;
    }
    default:
        break;
    }
}

static void ASIO_ReadChannel(const void *buffer, ASIOSampleType type, float *dst, int stride, int frames)
{
    const int bits = ASIO_GetSampleBits(type);
    int i;

    switch (type) {
    case ASIOSTInt16LSB:
    {
        const Sint16 *src = (const Sint16 *)buffer;
        for (i = 0; i < frames; ++i, dst += stride) {
            *dst = ASIO_IntToFloat(src[i], bits);
        }
        break;
    }
    case ASIOSTInt24LSB:
    {
        const Uint8 *src = (const Uint8 *)buffer;
        for (i = 0; i < frames; ++i, src += 3, dst += stride) {
            const Sint32 value = (Sint32)(((Uint32)src[0] << 8) | ((Uint32)src[1] << 16) | ((Uint32)src[2] << 24)) >> 8;
            *dst = ASIO_IntToFloat(value, bits);
        }
        break;
    }
    case ASIOSTInt32LSB:
    case ASIOSTInt32LSB16:
    case ASIOSTInt32LSB18:
    case ASIOSTInt32LSB20:
    case ASIOSTInt32LSB24:
    {
        const Sint32 *src = (const Sint32 *)buffer;
        for (i = 0; i < frames; ++i, dst += stride) {
            *dst = ASIO_IntToFloat(src[i], bits);
        }
        break;
    }
    case ASIOSTFloat32LSB:
    {
        const float *src = (const float *)buffer;
        for (i = 0; i < frames; ++i, dst += stride) {
            *dst = src[i];
        }
        break;
    }
    case ASIOSTFloat64LSB:
    {
        const double *src = (const double *)buffer;
        for (i = 0; i < frames; ++i, dst += stride) {
            *dst = (float)src[i];
        }
        break;
    }
    default:
        break;
    }
}

static int ASIO_GetSampleSize(ASIOSampleType type)
{
    switch (type) {
    case ASIOSTInt16LSB:
        return 2;
    case ASIOSTInt24LSB:
        return 3;
    case ASIOSTFloat64LSB:
        return 8;
    default:
        return 4;
    }
}

static void ASIO_SilenceOutputs(long index)
{
    for (int i = 0; i < asio.playback_channels; ++i) {
        SDL_memset(asio.buffer_infos[i].buffers[index], 0, (size_t)asio.buffer_frames * ASIO_GetSampleSize(asio.sample_types[i]));
    }
}

static void ASIO_ProcessBuffers(long index)
{
    asio.buffer_index = index;

    // Record first so a full duplex app can pass input through in the same period.
    if (asio.recording) {
        SDL_RecordingAudioThreadIterate(asio.recording);
    }

    if (asio.playback) {
        asio.wrote_output = false;
        SDL_PlaybackAudioThreadIterate(asio.playback);
        if (!asio.wrote_output) {
            ASIO_SilenceOutputs(index);
        }
    }

    if (asio.post_output) {
        IASIO_outputReady(asio.iasio);
    }
}

static void __cdecl ASIO_BufferSwitch(long index, ASIOBool directProcess)
{
    ASIO_ProcessBuffers(index);
}

static ASIOTime *__cdecl ASIO_BufferSwitchTimeInfo(ASIOTime *params, long index, ASIOBool directProcess)
{
    ASIO_ProcessBuffers(index);
    return params;
}

static void __cdecl ASIO_SampleRateDidChange(ASIOSampleRate rate)
{
    SDL_LogDebug(SDL_LOG_CATEGORY_AUDIO, "ASIO: sample rate changed to %g", rate);

    asio.sample_rate = rate;

    SDL_AudioDevice *devices[] = { asio.playback, asio.recording };
    for (int i = 0; i < (int)SDL_arraysize(devices); ++i) {
        SDL_AudioDevice *device = devices[i];
        if (device) {
            SDL_AudioSpec spec;
            SDL_copyp(&spec, &device->spec);
            spec.freq = (int)rate;
            if (!SDL_AudioDeviceFormatChanged(device, &spec, device->sample_frames)) {
                SDL_AudioDeviceDisconnected(device);
            }
        }
    }
}

static long __cdecl ASIO_Message(long selector, long value, void *message, double *opt)
{
    switch (selector) {
    case kAsioSelectorSupported:
        switch (value) {
        case kAsioEngineVersion:
        case kAsioResetRequest:
        case kAsioResyncRequest:
        case kAsioLatenciesChanged:
        case kAsioSupportsTimeInfo:
        case kAsioOverload:
            return 1;
        default:
            return 0;
        }

    case kAsioEngineVersion:
        return 2;

    case kAsioResetRequest:
        // The driver settings changed (usually from its control panel) and it needs to be
        // reopened. We can't do that from the driver thread, so report the devices as lost.
        // FIXME: Reopen the driver and report a format change instead.
        SDL_LogDebug(SDL_LOG_CATEGORY_AUDIO, "ASIO: driver requested reset");
        if (asio.playback) {
            SDL_AudioDeviceDisconnected(asio.playback);
        }
        if (asio.recording) {
            SDL_AudioDeviceDisconnected(asio.recording);
        }
        return 1;

    case kAsioResyncRequest:
        return 1;

    case kAsioLatenciesChanged:
    {
        long input_latency = 0, output_latency = 0;
        if (IASIO_getLatencies(asio.iasio, &input_latency, &output_latency) == ASE_OK) {
            SDL_LogDebug(SDL_LOG_CATEGORY_AUDIO, "ASIO: latencies changed: input %ld frames, output %ld frames", input_latency, output_latency);
        }
        return 1;
    }

    case kAsioSupportsTimeInfo:
        return 0;

    case kAsioOverload:
        SDL_AddAtomicInt(&asio.overloads, 1);
        return 1;

    default:
        return 0;
    }
}

static IASIO *ASIO_CreateDriver(const CLSID *clsid)
{
    IASIO *iasio = NULL;

    // ASIO drivers use their CLSID as the interface ID as well.
    HRESULT hr = CoCreateInstance(clsid, NULL, CLSCTX_INPROC_SERVER, clsid, (void **)&iasio);
    if (FAILED(hr)) {
        WIN_SetErrorFromHRESULT("ASIO: CoCreateInstance", hr);
        return NULL;
    }

    if (!IASIO_init(iasio, GetDesktopWindow())) {
        char message[128];
        SDL_zeroa(message);
        IASIO_getErrorMessage(iasio, message);
        message[sizeof(message) - 1] = '\0';
        SDL_SetError("ASIO: driver init failed: %s", message);
        IASIO_Release(iasio);
        return NULL;
    }
    return iasio;
}

// Drivers aren't loaded until they're opened, so the channel counts and sample rate aren't known yet.
// Some drivers don't work correctly if they're loaded more than once.
static void ASIO_AddDriver(const CLSID *clsid, const WCHAR *description, const WCHAR *clsid_string)
{
    ASIO_DriverHandle *handle = NULL;
    ASIO_DriverHandle **handles;
    char *name = WIN_StringToUTF8W(description);
    char *unique_id = WIN_StringToUTF8W(clsid_string);

    if (!name || !unique_id) {
        goto done;
    }

    handles = (ASIO_DriverHandle **)SDL_realloc(asio.handles, (asio.num_handles + 1) * sizeof(*handles));
    if (!handles) {
        goto done;
    }
    asio.handles = handles;

    handle = (ASIO_DriverHandle *)SDL_calloc(1, sizeof(*handle));
    if (!handle) {
        goto done;
    }
    handle->clsid = *clsid;
    handle->name = name;
    handle->playback.driver = handle;
    handle->recording.driver = handle;
    name = NULL;
    asio.handles[asio.num_handles++] = handle;

    SDL_LogDebug(SDL_LOG_CATEGORY_AUDIO, "ASIO: found '%s'", handle->name);

    SDL_AudioSpec spec;
    SDL_zero(spec);
    spec.format = SDL_AUDIO_F32;
    SDL_AddAudioDevice(false, handle->name, NULL, unique_id, &spec, &handle->playback);
    SDL_AddAudioDevice(true, handle->name, NULL, unique_id, &spec, &handle->recording);

done:
    SDL_free(name);
    SDL_free(unique_id);
}

static void ASIO_DetectDevices(SDL_AudioDevice **default_playback, SDL_AudioDevice **default_recording)
{
    HKEY hkey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &hkey) != ERROR_SUCCESS) {
        return;
    }

    for (DWORD i = 0;; ++i) {
        WCHAR keyname[256];
        DWORD keyname_length = SDL_arraysize(keyname);
        LONG rc = RegEnumKeyExW(hkey, i, keyname, &keyname_length, NULL, NULL, NULL, NULL);
        if (rc == ERROR_NO_MORE_ITEMS) {
            break;
        } else if (rc != ERROR_SUCCESS) {
            continue;
        }

        WCHAR clsid_string[64];
        DWORD size = sizeof(clsid_string);
        if (RegGetValueW(hkey, keyname, L"CLSID", RRF_RT_REG_SZ, NULL, clsid_string, &size) != ERROR_SUCCESS) {
            continue;
        }

        CLSID clsid;
        if (FAILED(CLSIDFromString(clsid_string, &clsid))) {
            continue;
        }

        WCHAR description[256];
        size = sizeof(description);
        if (RegGetValueW(hkey, keyname, L"Description", RRF_RT_REG_SZ, NULL, description, &size) != ERROR_SUCCESS) {
            SDL_wcslcpy(description, keyname, SDL_arraysize(description));
        }

        ASIO_AddDriver(&clsid, description, clsid_string);
    }

    RegCloseKey(hkey);
}

static long ASIO_ChooseBufferSize(long min_size, long max_size, long preferred_size, long granularity)
{
    const char *hint = SDL_GetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES);
    long frames = hint ? SDL_atoi(hint) : 0;

    if (frames <= 0) {
        return preferred_size;
    }

    frames = SDL_clamp(frames, min_size, max_size);
    if (granularity == -1) {
        // Powers of two only, pick the smallest one that holds the requested size.
        long size = min_size;
        while (size < frames && size * 2 <= max_size) {
            size *= 2;
        }
        frames = size;
    } else if (granularity > 0) {
        frames = min_size + ((frames - min_size + granularity - 1) / granularity) * granularity;
        frames = SDL_min(frames, max_size);
    } else {
        frames = preferred_size;
    }
    return frames;
}

static void ASIO_UnloadDriver(void)
{
    if (asio.iasio) {
        IASIO_Release(asio.iasio);
        asio.iasio = NULL;
    }
    // FIXME: This assumes the driver is unloaded on the thread that loaded it.
    if (asio.com_initialized) {
        WIN_CoUninitialize();
        asio.com_initialized = false;
    }
    asio.loaded_handle = NULL;
}

static bool ASIO_LoadDriver(ASIO_DriverHandle *handle, int freq)
{
    long min_size = 0, max_size = 0, preferred_size = 0, granularity = 0;
    ASIOSampleRate rate = 0.0;

    asio.com_initialized = SUCCEEDED(WIN_CoInitialize());

    asio.iasio = ASIO_CreateDriver(&handle->clsid);
    if (!asio.iasio) {
        goto failed;
    }
    asio.loaded_handle = handle;

    if (IASIO_getChannels(asio.iasio, &asio.num_inputs, &asio.num_outputs) != ASE_OK) {
        SDL_SetError("ASIO: couldn't get channel count");
        goto failed;
    }

    // Keep the hardware at the rate it's configured for and let SDL resample,
    // unless the driver doesn't have a rate yet.
    ASIOError result = IASIO_getSampleRate(asio.iasio, &rate);
    if (result != ASE_OK || rate <= 0.0) {
        const double rates[] = { (double)freq, 48000.0, 44100.0 };

        SDL_LogDebug(SDL_LOG_CATEGORY_AUDIO, "ASIO: driver has no sample rate (error %ld, rate %g)", result, rate);

        rate = 0.0;
        for (int i = 0; i < (int)SDL_arraysize(rates); ++i) {
            if (rates[i] <= 0.0) {
                continue;
            }
            result = IASIO_canSampleRate(asio.iasio, rates[i]);
            if (result != ASE_OK) {
                SDL_LogDebug(SDL_LOG_CATEGORY_AUDIO, "ASIO: sample rate %g not supported (error %ld)", rates[i], result);
                continue;
            }
            result = IASIO_setSampleRate(asio.iasio, rates[i]);
            if (result != ASE_OK) {
                SDL_LogDebug(SDL_LOG_CATEGORY_AUDIO, "ASIO: couldn't set sample rate to %g (error %ld)", rates[i], result);
                continue;
            }
            if (IASIO_getSampleRate(asio.iasio, &rate) != ASE_OK || rate <= 0.0) {
                rate = rates[i];
            }
            break;
        }
        if (rate <= 0.0) {
            SDL_SetError("ASIO: couldn't get sample rate (error %ld)", result);
            goto failed;
        }
    }
    asio.sample_rate = rate;

    if (IASIO_getBufferSize(asio.iasio, &min_size, &max_size, &preferred_size, &granularity) != ASE_OK || preferred_size <= 0) {
        SDL_SetError("ASIO: couldn't get buffer size");
        goto failed;
    }
    asio.buffer_frames = ASIO_ChooseBufferSize(min_size, max_size, preferred_size, granularity);

    asio.post_output = (IASIO_outputReady(asio.iasio) == ASE_OK);

    SDL_LogDebug(SDL_LOG_CATEGORY_AUDIO, "ASIO: loaded '%s', %ld inputs, %ld outputs, %g Hz, buffer sizes min %ld max %ld preferred %ld granularity %ld, using %ld, outputReady %s",
                 handle->name, asio.num_inputs, asio.num_outputs, rate, min_size, max_size, preferred_size, granularity, asio.buffer_frames, asio.post_output ? "supported" : "not supported");

    SDL_SetAtomicInt(&asio.overloads, 0);
    return true;

failed:
    ASIO_UnloadDriver();
    return false;
}

static void ASIO_StopDriver(void)
{
    if (asio.running) {
        IASIO_stop(asio.iasio);
        asio.running = false;
    }
    if (asio.buffers_created) {
        IASIO_disposeBuffers(asio.iasio);
        asio.buffers_created = false;
    }
}

static bool ASIO_CreateBuffers(void)
{
    const int num_channels = asio.playback_channels + asio.recording_channels;
    int i;

    for (i = 0; i < num_channels; ++i) {
        ASIOBufferInfo *info = &asio.buffer_infos[i];
        const bool input = (i >= asio.playback_channels);
        SDL_zerop(info);
        info->isInput = input ? ASIOTrue : ASIOFalse;
        info->channelNum = input ? (i - asio.playback_channels) : i;
    }

    asio.callbacks.bufferSwitch = ASIO_BufferSwitch;
    asio.callbacks.sampleRateDidChange = ASIO_SampleRateDidChange;
    asio.callbacks.asioMessage = ASIO_Message;
    asio.callbacks.bufferSwitchTimeInfo = ASIO_BufferSwitchTimeInfo;

    ASIOError result = IASIO_createBuffers(asio.iasio, asio.buffer_infos, num_channels, asio.buffer_frames, &asio.callbacks);
    if (result != ASE_OK) {
        return SDL_SetError("ASIO: createBuffers failed (%ld)", result);
    }
    asio.buffers_created = true;

    for (i = 0; i < num_channels; ++i) {
        ASIOChannelInfo info;
        SDL_zero(info);
        info.channel = asio.buffer_infos[i].channelNum;
        info.isInput = asio.buffer_infos[i].isInput;
        if (IASIO_getChannelInfo(asio.iasio, &info) != ASE_OK) {
            ASIO_StopDriver();
            return SDL_SetError("ASIO: getChannelInfo failed");
        }
        if (ASIO_GetSampleBits(info.type) == 0) {
            ASIO_StopDriver();
            return SDL_SetError("ASIO: unsupported sample type %ld", info.type);
        }
        asio.sample_types[i] = info.type;
    }

    long input_latency = 0, output_latency = 0;
    IASIO_getLatencies(asio.iasio, &input_latency, &output_latency);
    SDL_LogDebug(SDL_LOG_CATEGORY_AUDIO, "ASIO: %d output and %d input channels, %ld frames (%.2f ms), sample type %ld, reported latency input %ld frames (%.2f ms), output %ld frames (%.2f ms)",
                 asio.playback_channels, asio.recording_channels,
                 asio.buffer_frames, asio.buffer_frames * 1000.0 / asio.sample_rate,
                 asio.sample_types[0],
                 input_latency, input_latency * 1000.0 / asio.sample_rate,
                 output_latency, output_latency * 1000.0 / asio.sample_rate);
    return true;
}

static bool ASIO_StartDriver(void)
{
    if (!asio.running) {
        ASIOError result = IASIO_start(asio.iasio);
        if (result != ASE_OK) {
            return SDL_SetError("ASIO: start failed (%ld)", result);
        }
        asio.running = true;
    }
    return true;
}

// Called with the lock held after a device was removed, to bring the driver back up for whatever is still open.
static void ASIO_ResumeDriver(void)
{
    if (!asio.playback && !asio.recording) {
        ASIO_UnloadDriver();
        return;
    }

    if (!ASIO_CreateBuffers() || !ASIO_StartDriver()) {
        SDL_LogDebug(SDL_LOG_CATEGORY_AUDIO, "ASIO: couldn't restart driver: %s", SDL_GetError());
        ASIO_StopDriver();
        if (asio.playback) {
            SDL_AudioDeviceDisconnected(asio.playback);
        }
        if (asio.recording) {
            SDL_AudioDeviceDisconnected(asio.recording);
        }
    }
}

static bool ASIO_OpenDevice(SDL_AudioDevice *device)
{
    ASIO_DriverHandle *handle = ((ASIO_DeviceHandle *)device->handle)->driver;
    const bool recording = device->recording;
    bool result = false;

    device->hidden = (struct SDL_PrivateAudioData *)SDL_calloc(1, sizeof(*device->hidden));
    if (!device->hidden) {
        return false;
    }

    SDL_LockMutex(asio.lock);

    if (asio.iasio && asio.loaded_handle != handle) {
        SDL_SetError("ASIO: only one driver can be open at a time, '%s' is already open", asio.loaded_handle->name);
        goto done;
    }

    if (asio.iasio) {
        // The other direction is running, we'll restart with both sets of buffers.
        ASIO_StopDriver();
    } else if (!ASIO_LoadDriver(handle, device->spec.freq)) {
        goto done;
    }

    const long available = recording ? asio.num_inputs : asio.num_outputs;
    if (available <= 0) {
        SDL_SetError("ASIO: driver has no %s channels", recording ? "input" : "output");
        ASIO_ResumeDriver();
        goto done;
    }

    // The first two outputs are left and right, so play at least stereo and let SDL upmix mono.
    const int requested = recording ? device->spec.channels : SDL_max(device->spec.channels, 2);
    const int channels = SDL_clamp(requested, 1, (int)SDL_min(available, SDL_MAX_CHANNELMAP_CHANNELS));
    device->spec.format = SDL_AUDIO_F32;
    device->spec.channels = channels;
    device->spec.freq = (int)asio.sample_rate;
    device->sample_frames = (int)asio.buffer_frames;
    SDL_UpdatedAudioDeviceFormat(device);

    if (!recording) {
        device->hidden->iobuffer = (float *)SDL_calloc(1, device->buffer_size);
        if (!device->hidden->iobuffer) {
            ASIO_ResumeDriver();
            goto done;
        }
    }

    if (recording) {
        asio.recording = device;
        asio.recording_channels = channels;
    } else {
        asio.playback = device;
        asio.playback_channels = channels;
    }

    if (!ASIO_CreateBuffers()) {
        if (recording) {
            asio.recording = NULL;
            asio.recording_channels = 0;
        } else {
            asio.playback = NULL;
            asio.playback_channels = 0;
        }
        ASIO_ResumeDriver();
        goto done;
    }

    // The driver is started in PostOpenDevice, without the device lock held.
    result = true;

done:
    SDL_UnlockMutex(asio.lock);
    return result;
}

static bool ASIO_PostOpenDevice(SDL_AudioDevice *device)
{
    SDL_LockMutex(asio.lock);
    const bool result = ASIO_StartDriver();
    SDL_UnlockMutex(asio.lock);
    return result;
}

static Uint8 *ASIO_GetDeviceBuf(SDL_AudioDevice *device, int *buffer_size)
{
    return (Uint8 *)device->hidden->iobuffer;
}

static bool ASIO_PlayDevice(SDL_AudioDevice *device, const Uint8 *buffer, int buflen)
{
    const float *src = (const float *)buffer;
    const int channels = asio.playback_channels;
    const int frames = (int)asio.buffer_frames;

    for (int i = 0; i < channels; ++i) {
        ASIO_WriteChannel(asio.buffer_infos[i].buffers[asio.buffer_index], asio.sample_types[i], src + i, channels, frames);
    }
    asio.wrote_output = true;
    return true;
}

static int ASIO_RecordDevice(SDL_AudioDevice *device, void *buffer, int buflen)
{
    float *dst = (float *)buffer;
    const int channels = asio.recording_channels;
    const int frames = (int)asio.buffer_frames;

    for (int i = 0; i < channels; ++i) {
        const int index = asio.playback_channels + i;
        ASIO_ReadChannel(asio.buffer_infos[index].buffers[asio.buffer_index], asio.sample_types[index], dst + i, channels, frames);
    }
    return frames * channels * (int)sizeof(float);
}

static void ASIO_FlushRecording(SDL_AudioDevice *device)
{
    // Nothing to do, the data is replaced on the next buffer switch.
}

static void ASIO_CloseDevice(SDL_AudioDevice *device)
{
    if (!device->hidden) {
        return;
    }

    SDL_LockMutex(asio.lock);
    if (asio.playback == device || asio.recording == device) {
        ASIO_StopDriver();

        const int overloads = SDL_GetAtomicInt(&asio.overloads);
        if (overloads > 0) {
            SDL_LogDebug(SDL_LOG_CATEGORY_AUDIO, "ASIO: driver reported %d overloads", overloads);
        }

        if (asio.playback == device) {
            asio.playback = NULL;
            asio.playback_channels = 0;
        } else {
            asio.recording = NULL;
            asio.recording_channels = 0;
        }
        ASIO_ResumeDriver();
    }
    SDL_UnlockMutex(asio.lock);

    SDL_free(device->hidden->iobuffer);
    SDL_free(device->hidden);
    device->hidden = NULL;
}

static void ASIO_Deinitialize(void)
{
    for (int i = 0; i < asio.num_handles; ++i) {
        SDL_free(asio.handles[i]->name);
        SDL_free(asio.handles[i]);
    }
    SDL_free(asio.handles);
    SDL_DestroyMutex(asio.lock);
    SDL_zero(asio);
}

static bool ASIO_Init(SDL_AudioDriverImpl *impl)
{
    asio.lock = SDL_CreateMutex();
    if (!asio.lock) {
        return false;
    }

    impl->DetectDevices = ASIO_DetectDevices;
    impl->OpenDevice = ASIO_OpenDevice;
    impl->PostOpenDevice = ASIO_PostOpenDevice;
    impl->GetDeviceBuf = ASIO_GetDeviceBuf;
    impl->PlayDevice = ASIO_PlayDevice;
    impl->RecordDevice = ASIO_RecordDevice;
    impl->FlushRecording = ASIO_FlushRecording;
    impl->CloseDevice = ASIO_CloseDevice;
    impl->Deinitialize = ASIO_Deinitialize;

    impl->HasRecordingSupport = true;
    impl->ProvidesOwnCallbackThread = true;

    return true;
}

AudioBootStrap ASIO_bootstrap = {
    "asio", "ASIO", ASIO_Init, true, false
};

#endif // SDL_AUDIO_DRIVER_ASIO
