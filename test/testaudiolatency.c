/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

// Measures audio round trip latency: plays short clicks on a playback device
// and times how long it takes for them to show up on a recording device.
//
// Connect an output to an input with a loopback cable (or hold a microphone
// against a speaker) and compare drivers at the same buffer size, e.g.:
//   testaudiolatency --audio wasapi --frames 128
//   testaudiolatency --audio asio --frames 128 --log audio

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_test.h>

#define CLICK_FRAMES_MS 1

typedef struct
{
    SDL_Mutex *lock;

    int playback_freq;
    int playback_channels;
    int recording_freq;
    int recording_channels;

    int interval_frames;
    int frames_until_click;
    int click_frames;
    int click_position;

    float threshold;
    int holdoff_frames;
    int holdoff;
    float noise_peak;

    Uint64 emit_time;
    bool emit_pending;
    int clicks_emitted;
    int clicks_missed;

    double *latencies;
    int num_latencies;
    int max_latencies;
} LatencyTest;

static LatencyTest test;

static void SDLCALL PlaybackCallback(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount)
{
    const Uint64 now = SDL_GetTicksNS();
    const int channels = test.playback_channels;
    const int frames = additional_amount / (int)(sizeof(float) * channels);
    float *buffer;

    if (frames <= 0) {
        return;
    }

    buffer = (float *)SDL_stack_alloc(float, frames * channels);
    if (!buffer) {
        return;
    }

    SDL_LockMutex(test.lock);
    for (int i = 0; i < frames; ++i) {
        float sample = 0.0f;

        if (test.frames_until_click == 0) {
            if (test.emit_pending) {
                ++test.clicks_missed;
            }
            test.emit_time = now + SDL_SECONDS_TO_NS(i) / test.playback_freq;
            test.emit_pending = true;
            ++test.clicks_emitted;
            test.click_position = 0;
            test.frames_until_click = test.interval_frames;
        }
        --test.frames_until_click;

        if (test.click_position < test.click_frames) {
            // One cycle of a square wave, for a sharp edge without a DC offset.
            sample = (test.click_position < test.click_frames / 2) ? 0.8f : -0.8f;
            ++test.click_position;
        }

        for (int c = 0; c < channels; ++c) {
            buffer[i * channels + c] = sample;
        }
    }
    SDL_UnlockMutex(test.lock);

    SDL_PutAudioStreamData(stream, buffer, frames * channels * (int)sizeof(float));
    SDL_stack_free(buffer);
}

static void SDLCALL RecordingCallback(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount)
{
    const Uint64 now = SDL_GetTicksNS();
    const int channels = test.recording_channels;
    float *buffer;
    int frames;

    if (additional_amount <= 0) {
        return;
    }

    buffer = (float *)SDL_stack_alloc(Uint8, additional_amount);
    if (!buffer) {
        return;
    }

    frames = SDL_GetAudioStreamData(stream, buffer, additional_amount) / (int)(sizeof(float) * channels);

    SDL_LockMutex(test.lock);
    for (int i = 0; i < frames; ++i) {
        float level = 0.0f;
        for (int c = 0; c < channels; ++c) {
            level = SDL_max(level, SDL_fabsf(buffer[i * channels + c]));
        }

        if (test.holdoff > 0) {
            --test.holdoff;
            continue;
        }

        if (level < test.threshold) {
            test.noise_peak = SDL_max(test.noise_peak, level);
            continue;
        }

        // The last sample in this buffer was captured roughly now.
        const Uint64 detect_time = now - SDL_SECONDS_TO_NS(frames - i) / test.recording_freq;
        if (test.emit_pending && detect_time >= test.emit_time) {
            if (test.num_latencies < test.max_latencies) {
                const double latency = (double)(detect_time - test.emit_time) / SDL_NS_PER_MS;
                test.latencies[test.num_latencies++] = latency;
                SDL_Log("Click %d: %.2f ms (level %.2f)", test.num_latencies, latency, level);
            }
            test.emit_pending = false;
        }
        test.holdoff = test.holdoff_frames;
    }
    SDL_UnlockMutex(test.lock);

    SDL_stack_free(buffer);
}

static int SDLCALL CompareDoubles(const void *a, const void *b)
{
    const double x = *(const double *)a;
    const double y = *(const double *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

static SDL_AudioDeviceID FindDevice(bool recording, int index)
{
    SDL_AudioDeviceID result;
    int count = 0;
    SDL_AudioDeviceID *devices;

    if (index < 0) {
        return recording ? SDL_AUDIO_DEVICE_DEFAULT_RECORDING : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    }

    devices = recording ? SDL_GetAudioRecordingDevices(&count) : SDL_GetAudioPlaybackDevices(&count);
    result = (devices && index < count) ? devices[index] : 0;
    SDL_free(devices);
    return result;
}

static void ListDevices(bool recording)
{
    int count = 0;
    SDL_AudioDeviceID *devices = recording ? SDL_GetAudioRecordingDevices(&count) : SDL_GetAudioPlaybackDevices(&count);

    SDL_Log("%s devices:", recording ? "Recording" : "Playback");
    for (int i = 0; i < count; ++i) {
        SDL_Log("  %d: %s", i, SDL_GetAudioDeviceName(devices[i]));
    }
    SDL_free(devices);
}

static SDL_AudioStream *OpenStream(bool recording, int index)
{
    const char *type = recording ? "recording" : "playback";
    SDL_AudioDeviceID devid = FindDevice(recording, index);
    SDL_AudioStream *stream;
    SDL_AudioSpec spec;
    int frames = 0;

    if (!devid) {
        SDL_Log("Couldn't find %s device %d", type, index);
        return NULL;
    }

    // Use the device format so the stream doesn't need to resample.
    if (!SDL_GetAudioDeviceFormat(devid, &spec, NULL)) {
        SDL_Log("Couldn't get %s device format: %s", type, SDL_GetError());
        return NULL;
    }
    spec.format = SDL_AUDIO_F32;

    stream = SDL_OpenAudioDeviceStream(devid, &spec, recording ? RecordingCallback : PlaybackCallback, NULL);
    if (!stream) {
        SDL_Log("Couldn't open %s device: %s", type, SDL_GetError());
        return NULL;
    }

    devid = SDL_GetAudioStreamDevice(stream);
    if (SDL_GetAudioDeviceFormat(devid, &spec, &frames)) {
        SDL_Log("%s: '%s', %d Hz, %d channels, %d frames (%.2f ms)", type, SDL_GetAudioDeviceName(devid), spec.freq, spec.channels, frames, frames * 1000.0 / spec.freq);
    }

    // Match the device, in case it opened with a different format than it reported.
    spec.format = SDL_AUDIO_F32;
    if (recording) {
        SDL_SetAudioStreamFormat(stream, NULL, &spec);
        test.recording_freq = spec.freq;
        test.recording_channels = spec.channels;
    } else {
        SDL_SetAudioStreamFormat(stream, &spec, NULL);
        test.playback_freq = spec.freq;
        test.playback_channels = spec.channels;
    }
    return stream;
}

int main(int argc, char **argv)
{
    SDLTest_CommonState *state;
    SDL_AudioStream *playback = NULL;
    SDL_AudioStream *recording = NULL;
    int playback_index = -1;
    int recording_index = -1;
    int num_clicks = 20;
    int interval_ms = 500;
    float threshold = 0.1f;
    bool list = false;
    int result = 1;
    int i;

    state = SDLTest_CommonCreateState(argv, SDL_INIT_AUDIO);
    if (!state) {
        return 1;
    }

    for (i = 1; i < argc;) {
        int consumed = SDLTest_CommonArg(state, i);
        if (consumed == 0) {
            consumed = -1;
            if (SDL_strcmp(argv[i], "--list") == 0) {
                list = true;
                consumed = 1;
            } else if (argv[i + 1]) {
                if (SDL_strcmp(argv[i], "--playback") == 0) {
                    playback_index = SDL_atoi(argv[i + 1]);
                    consumed = 2;
                } else if (SDL_strcmp(argv[i], "--recording") == 0) {
                    recording_index = SDL_atoi(argv[i + 1]);
                    consumed = 2;
                } else if (SDL_strcmp(argv[i], "--frames") == 0) {
                    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, argv[i + 1]);
                    consumed = 2;
                } else if (SDL_strcmp(argv[i], "--clicks") == 0) {
                    num_clicks = SDL_atoi(argv[i + 1]);
                    consumed = 2;
                } else if (SDL_strcmp(argv[i], "--interval") == 0) {
                    interval_ms = SDL_atoi(argv[i + 1]);
                    consumed = 2;
                } else if (SDL_strcmp(argv[i], "--threshold") == 0) {
                    threshold = (float)SDL_atof(argv[i + 1]);
                    consumed = 2;
                }
            }
        }
        if (consumed < 0) {
            static const char *options[] = {
                "[--list]", "[--playback N]", "[--recording N]", "[--frames N]",
                "[--clicks N]", "[--interval ms]", "[--threshold level]", NULL
            };
            SDLTest_CommonLogUsage(state, argv[0], options);
            return 1;
        }
        i += consumed;
    }

    if (!SDL_Init(SDL_INIT_AUDIO)) {
        SDL_Log("Couldn't initialize SDL: %s", SDL_GetError());
        return 1;
    }

    SDL_Log("Using audio driver: %s", SDL_GetCurrentAudioDriver());

    if (list) {
        ListDevices(false);
        ListDevices(true);
        result = 0;
        goto done;
    }

    test.lock = SDL_CreateMutex();
    test.threshold = threshold;
    test.max_latencies = SDL_max(num_clicks, 1);
    test.latencies = (double *)SDL_calloc(test.max_latencies, sizeof(double));
    if (!test.lock || !test.latencies) {
        goto done;
    }

    recording = OpenStream(true, recording_index);
    playback = OpenStream(false, playback_index);
    if (!recording || !playback) {
        goto done;
    }

    SDL_LockMutex(test.lock);
    test.interval_frames = (int)((Sint64)test.playback_freq * interval_ms / 1000);
    test.frames_until_click = test.interval_frames;
    test.click_frames = SDL_max(test.playback_freq * CLICK_FRAMES_MS / 1000, 2);
    test.click_position = test.click_frames;
    test.holdoff_frames = (int)((Sint64)test.recording_freq * interval_ms / 2000);
    SDL_UnlockMutex(test.lock);

    SDL_ResumeAudioStreamDevice(recording);
    SDL_ResumeAudioStreamDevice(playback);

    const Uint64 timeout = SDL_GetTicks() + (Uint64)(num_clicks + 4) * interval_ms + 2000;
    while (SDL_GetTicks() < timeout) {
        SDL_LockMutex(test.lock);
        const bool finished = (test.num_latencies >= test.max_latencies);
        SDL_UnlockMutex(test.lock);
        if (finished) {
            break;
        }
        SDL_Delay(50);
    }

    SDL_DestroyAudioStream(playback);
    playback = NULL;
    SDL_DestroyAudioStream(recording);
    recording = NULL;

    SDL_Log("Clicks played: %d, detected: %d, missed: %d, loudest non-click level: %.3f (threshold %.3f)",
            test.clicks_emitted, test.num_latencies, test.clicks_missed, test.noise_peak, test.threshold);

    if (test.num_latencies > 0) {
        double total = 0.0;
        for (i = 0; i < test.num_latencies; ++i) {
            total += test.latencies[i];
        }
        SDL_qsort(test.latencies, test.num_latencies, sizeof(double), CompareDoubles);
        SDL_Log("Round trip latency: min %.2f ms, median %.2f ms, mean %.2f ms, max %.2f ms",
                test.latencies[0], test.latencies[test.num_latencies / 2],
                total / test.num_latencies, test.latencies[test.num_latencies - 1]);
        result = 0;
    } else {
        SDL_Log("No clicks detected, check the loopback connection or lower --threshold");
    }

done:
    SDL_DestroyAudioStream(playback);
    SDL_DestroyAudioStream(recording);
    SDL_free(test.latencies);
    SDL_DestroyMutex(test.lock);
    SDL_Quit();
    SDLTest_CommonDestroyState(state);
    return result;
}
