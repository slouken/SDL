# ASIO audio driver (prototype)

This is an experimental ASIO audio driver for Windows, written to compare
latency with the WASAPI driver. It is a prototype and is not intended for
release in its current form.

## Using it

The driver is only used when requested explicitly:

```
SDL_AUDIO_DRIVER=asio
```

or `--audio asio` with the SDL test programs.

It is built by default on Windows, and can be disabled with the `SDL_ASIO`
CMake option. It is also included in the Visual Studio project.

## How it works

The source is in `src/audio/asio`.

- The ASIO SDK is not used. The driver's COM interface and structures are
  declared in `SDL_asio.h`.
- The driver methods use the C++ `__thiscall` calling convention, which C
  can't call directly. On 32-bit x86 they're declared as `__fastcall` with an
  unused second parameter, which is binary compatible. Other architectures
  have a single calling convention and need nothing special.
- Drivers are enumerated from `HKLM\SOFTWARE\ASIO`, and each driver appears as
  one playback device and one recording device. Drivers aren't loaded until a
  device is opened, because some drivers don't work correctly if they're
  loaded more than once. Until then the devices report SDL's default format,
  and a device fails to open if the driver has no channels in that direction.
- Only one ASIO driver can be loaded at a time. The ASIO callbacks have no user
  data, so all driver state is global. Opening a device on a second driver
  fails with an error.
- When both the playback and recording devices of a driver are open, they share
  one set of driver buffers and one buffer switch callback. Each callback
  handles recording first, then playback, then calls `outputReady()` if the
  driver supports it. Opening or closing one side stops the driver and
  recreates the buffers, so the other side drops out briefly.
- SDL always sees 32-bit float audio. Each channel is converted to and from the
  driver's sample type: 16, 24 and 32-bit integer, the 32-bit container
  variants (16, 18, 20 and 24-bit data), and 32 and 64-bit float. At most 8
  channels are used per side, taken from the first channels the driver offers.
  Playback uses at least 2 channels, so mono is played on both left and right.
- The hardware stays at the sample rate it's configured for, and SDL resamples
  to the rate the app asks for. A rate is only set if the driver reports none:
  the requested rate if the driver supports it, otherwise 48000 or 44100 Hz.

## Latency testing

### Buffer size

The driver uses `SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES` as the buffer size,
rounded up to a size the driver allows. Without the hint it uses the driver's
preferred buffer size.

The WASAPI driver uses the same hint, so both can run at the same buffer size.
WASAPI rounds the size to a multiple of the device's base period, which is
often 10 ms in shared mode, so check the frame counts the test reports.

### Logging

With audio debug logging enabled (`--log audio` in the test programs), the
driver logs:

- the buffer sizes the driver supports and the size chosen,
- the input and output latency the driver reports, in frames and ms,
- the number of overloads (dropouts) the driver reported, when the device
  closes.

### testaudiolatency

`test/testaudiolatency.c` measures round trip latency with any audio driver.
It plays a 1 ms click every 500 ms on a playback device, detects it on a
recording device, and reports the min, median, mean and max time between the
two. It needs a loopback cable from an output to an input, or a microphone held
against a speaker.

```
testaudiolatency --list
testaudiolatency --audio wasapi --frames 128
testaudiolatency --audio asio --frames 128 --log audio
```

Options:

- `--list`: list the playback and recording devices and exit
- `--playback N`, `--recording N`: device index from `--list`, the default
  devices are used otherwise
- `--frames N`: buffer size, sets `SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES`
- `--clicks N`: number of clicks to measure, default 20
- `--interval ms`: time between clicks, default 500
- `--threshold level`: detection level, default 0.1

The test also prints the loudest level seen outside of a click, which helps
pick a threshold if clicks aren't being detected.

Timestamps are taken when SDL asks for playback data and when recorded data
arrives, so each reading can be off by up to one buffer period. Compare the
medians.

The test is built with CMake. It isn't in the Visual Studio project.

## Known limitations

- If the driver requests a reset, for example after its settings are changed
  in its control panel, the devices are reported as disconnected instead of
  being reopened.
- COM is initialized on the thread that loads the driver and uninitialized on
  the thread that unloads it. This is wrong if those threads differ.
- Opening the playback and recording devices of the same driver from two
  threads at once could deadlock. Stopping the driver waits for the callback,
  which may be waiting for a device lock.
- Big-endian sample types aren't supported.

## Open questions

- Licensing: the interface declarations are written from scratch rather than
  taken from the ASIO SDK. Whether that is acceptable for zlib licensed SDL
  code needs to be decided.

## Status

The driver has only been syntax checked against MinGW headers. It has not been
built with Visual Studio or run against a real ASIO driver.
