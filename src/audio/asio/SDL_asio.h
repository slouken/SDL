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
#ifndef SDL_asio_h_
#define SDL_asio_h_

#include "../SDL_sysaudio.h"

// ASIO drivers are in-process COM objects implementing the IASIO C++ interface.
// The declarations below describe that binary interface so we don't need the ASIO SDK.

#pragma pack(push, 4)

typedef long ASIOBool;
typedef long ASIOError;
typedef long ASIOSampleType;
typedef double ASIOSampleRate;

#define ASIOFalse 0
#define ASIOTrue  1

#define ASE_OK                0
#define ASE_SUCCESS           0x3f4847a0
#define ASE_NotPresent        -1000
#define ASE_HWMalfunction     -999
#define ASE_InvalidParameter  -998
#define ASE_InvalidMode       -997
#define ASE_SPNotAdvancing    -996
#define ASE_NoClock           -995
#define ASE_NoMemory          -994

#define ASIOSTInt16LSB    16
#define ASIOSTInt24LSB    17
#define ASIOSTInt32LSB    18
#define ASIOSTFloat32LSB  19
#define ASIOSTFloat64LSB  20
#define ASIOSTInt32LSB16  24
#define ASIOSTInt32LSB18  25
#define ASIOSTInt32LSB20  26
#define ASIOSTInt32LSB24  27

#define kAsioSelectorSupported  1
#define kAsioEngineVersion      2
#define kAsioResetRequest       3
#define kAsioBufferSizeChange   4
#define kAsioResyncRequest      5
#define kAsioLatenciesChanged   6
#define kAsioSupportsTimeInfo   7
#define kAsioSupportsTimeCode   8
#define kAsioOverload           15

typedef struct ASIOChannelInfo
{
    long channel;
    ASIOBool isInput;
    ASIOBool isActive;
    long channelGroup;
    ASIOSampleType type;
    char name[32];
} ASIOChannelInfo;

typedef struct ASIOBufferInfo
{
    ASIOBool isInput;
    long channelNum;
    void *buffers[2];
} ASIOBufferInfo;

// We never look inside this, it's just passed back to the driver.
typedef struct ASIOTime ASIOTime;

typedef struct ASIOCallbacks
{
    void (__cdecl *bufferSwitch)(long doubleBufferIndex, ASIOBool directProcess);
    void (__cdecl *sampleRateDidChange)(ASIOSampleRate sRate);
    long (__cdecl *asioMessage)(long selector, long value, void *message, double *opt);
    ASIOTime *(__cdecl *bufferSwitchTimeInfo)(ASIOTime *params, long doubleBufferIndex, ASIOBool directProcess);
} ASIOCallbacks;

#pragma pack(pop)

// The IASIO methods are C++ __thiscall on 32-bit x86 (this in ECX, callee cleans the stack).
// __fastcall with an unused second parameter (EDX) is binary compatible and can be used from C.
// Every other architecture has a single calling convention.
#if defined(_M_IX86) || defined(__i386__)
#define ASIO_THISCALL        __fastcall
#define ASIO_THIS(T)         T *This, void *edx_unused
#define ASIO_PASS_THIS(p)    (p), NULL
#else
#define ASIO_THISCALL
#define ASIO_THIS(T)         T *This
#define ASIO_PASS_THIS(p)    (p)
#endif

typedef struct IASIO IASIO;

typedef struct IASIOVtbl
{
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IASIO *This, REFIID riid, void **ppvObject);
    ULONG (STDMETHODCALLTYPE *AddRef)(IASIO *This);
    ULONG (STDMETHODCALLTYPE *Release)(IASIO *This);
    ASIOBool (ASIO_THISCALL *init)(ASIO_THIS(IASIO), void *sysHandle);
    void (ASIO_THISCALL *getDriverName)(ASIO_THIS(IASIO), char *name);
    long (ASIO_THISCALL *getDriverVersion)(ASIO_THIS(IASIO));
    void (ASIO_THISCALL *getErrorMessage)(ASIO_THIS(IASIO), char *string);
    ASIOError (ASIO_THISCALL *start)(ASIO_THIS(IASIO));
    ASIOError (ASIO_THISCALL *stop)(ASIO_THIS(IASIO));
    ASIOError (ASIO_THISCALL *getChannels)(ASIO_THIS(IASIO), long *numInputChannels, long *numOutputChannels);
    ASIOError (ASIO_THISCALL *getLatencies)(ASIO_THIS(IASIO), long *inputLatency, long *outputLatency);
    ASIOError (ASIO_THISCALL *getBufferSize)(ASIO_THIS(IASIO), long *minSize, long *maxSize, long *preferredSize, long *granularity);
    ASIOError (ASIO_THISCALL *canSampleRate)(ASIO_THIS(IASIO), ASIOSampleRate sampleRate);
    ASIOError (ASIO_THISCALL *getSampleRate)(ASIO_THIS(IASIO), ASIOSampleRate *sampleRate);
    ASIOError (ASIO_THISCALL *setSampleRate)(ASIO_THIS(IASIO), ASIOSampleRate sampleRate);
    ASIOError (ASIO_THISCALL *getClockSources)(ASIO_THIS(IASIO), void *clocks, long *numSources);
    ASIOError (ASIO_THISCALL *setClockSource)(ASIO_THIS(IASIO), long reference);
    ASIOError (ASIO_THISCALL *getSamplePosition)(ASIO_THIS(IASIO), void *sPos, void *tStamp);
    ASIOError (ASIO_THISCALL *getChannelInfo)(ASIO_THIS(IASIO), ASIOChannelInfo *info);
    ASIOError (ASIO_THISCALL *createBuffers)(ASIO_THIS(IASIO), ASIOBufferInfo *bufferInfos, long numChannels, long bufferSize, ASIOCallbacks *callbacks);
    ASIOError (ASIO_THISCALL *disposeBuffers)(ASIO_THIS(IASIO));
    ASIOError (ASIO_THISCALL *controlPanel)(ASIO_THIS(IASIO));
    ASIOError (ASIO_THISCALL *future)(ASIO_THIS(IASIO), long selector, void *opt);
    ASIOError (ASIO_THISCALL *outputReady)(ASIO_THIS(IASIO));
} IASIOVtbl;

struct IASIO
{
    const IASIOVtbl *lpVtbl;
};

#define IASIO_Release(p)                    (p)->lpVtbl->Release(p)
#define IASIO_init(p, a)                    (p)->lpVtbl->init(ASIO_PASS_THIS(p), a)
#define IASIO_getErrorMessage(p, a)         (p)->lpVtbl->getErrorMessage(ASIO_PASS_THIS(p), a)
#define IASIO_start(p)                      (p)->lpVtbl->start(ASIO_PASS_THIS(p))
#define IASIO_stop(p)                       (p)->lpVtbl->stop(ASIO_PASS_THIS(p))
#define IASIO_getChannels(p, a, b)          (p)->lpVtbl->getChannels(ASIO_PASS_THIS(p), a, b)
#define IASIO_getLatencies(p, a, b)         (p)->lpVtbl->getLatencies(ASIO_PASS_THIS(p), a, b)
#define IASIO_getBufferSize(p, a, b, c, d)  (p)->lpVtbl->getBufferSize(ASIO_PASS_THIS(p), a, b, c, d)
#define IASIO_canSampleRate(p, a)           (p)->lpVtbl->canSampleRate(ASIO_PASS_THIS(p), a)
#define IASIO_getSampleRate(p, a)           (p)->lpVtbl->getSampleRate(ASIO_PASS_THIS(p), a)
#define IASIO_setSampleRate(p, a)           (p)->lpVtbl->setSampleRate(ASIO_PASS_THIS(p), a)
#define IASIO_getChannelInfo(p, a)          (p)->lpVtbl->getChannelInfo(ASIO_PASS_THIS(p), a)
#define IASIO_createBuffers(p, a, b, c, d)  (p)->lpVtbl->createBuffers(ASIO_PASS_THIS(p), a, b, c, d)
#define IASIO_disposeBuffers(p)             (p)->lpVtbl->disposeBuffers(ASIO_PASS_THIS(p))
#define IASIO_outputReady(p)                (p)->lpVtbl->outputReady(ASIO_PASS_THIS(p))

typedef struct ASIO_DriverHandle ASIO_DriverHandle;

// SDL requires a unique handle per device, so playback and recording each get one.
typedef struct ASIO_DeviceHandle
{
    ASIO_DriverHandle *driver;
} ASIO_DeviceHandle;

// One of these per installed ASIO driver, shared by its playback and recording devices.
struct ASIO_DriverHandle
{
    CLSID clsid;
    char *name;
    ASIO_DeviceHandle playback;
    ASIO_DeviceHandle recording;
};

struct SDL_PrivateAudioData
{
    float *iobuffer;
};

#endif // SDL_asio_h_
