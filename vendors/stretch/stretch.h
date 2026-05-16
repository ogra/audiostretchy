////////////////////////////////////////////////////////////////////////////
//                        **** AUDIO-STRETCH ****                         //
//                      Time Domain Harmonic Scaler                       //
//                    Copyright (c) 2022 David Bryant                     //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// stretch.h

// Time Domain Harmonic Compression and Expansion
//
// This library performs time domain harmonic scaling with pitch detection
// to stretch the timing of a PCM or IEEE Float signal (either mono or stereo) from
// 1/2 to 2 times its original length. This is done without altering any of
// its tonal characteristics.
//
// Use stereo (num_chans = 2), when both channels are from same source
// and should contain approximately similar content.
// For independent channels, prefer using multiple StretchHandle-instances.
// see https://github.com/dbry/audio-stretch/issues/6

#ifndef STRETCH_H
#define STRETCH_H

#include <stdint.h>

#define STRETCH_FAST_FLAG    0x1    // use "fast" version of period determination code
#define STRETCH_DUAL_FLAG    0x2    // cascade two instances (doubles usable ratio range)

#ifndef STRETCH_API
#ifdef _WIN32
#define STRETCH_API __declspec(dllexport)
#else
#define STRETCH_API
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void *StretchHandle;

STRETCH_API StretchHandle stretch_init (int shortest_period, int longest_period, int num_chans, int flags);
STRETCH_API int stretch_output_capacity (StretchHandle handle, int max_num_samples, float max_ratio);

// Original int16 API (maintained for backward compatibility)
STRETCH_API int stretch_samples (StretchHandle handle, const int16_t *samples, int num_samples, int16_t *output, float ratio);
STRETCH_API int stretch_flush (StretchHandle handle, int16_t *output);

// New float32 API (preferred for new code)
STRETCH_API int stretch_samples_float (StretchHandle handle, const float *samples, int num_samples, float *output, float ratio);
STRETCH_API int stretch_flush_float (StretchHandle handle, float *output);

STRETCH_API void stretch_reset (StretchHandle handle);
STRETCH_API void stretch_deinit (StretchHandle handle);

#ifdef __cplusplus
}
#endif

#endif

