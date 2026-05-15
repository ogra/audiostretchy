////////////////////////////////////////////////////////////////////////////
//                        **** AUDIO-STRETCH ****                         //
//                      Time Domain Harmonic Scaler                       //
//                    Copyright (c) 2022 David Bryant                     //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// stretch.c

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


#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "stretch.h"

#define MIN_PERIOD  24          /* minimum allowable pitch period */
#define MAX_PERIOD  2400        /* maximum allowable pitch period */

struct stretch_cnxt {
    int num_chans, inbuff_samples, shortest, longest, tail, head, fast_mode;
    float *inbuff, *calcbuff;
    float outsamples_error;
    float *results;

    struct stretch_cnxt *next;
    float *intermediate;
};

static void merge_blocks_float (float *output, const float *input1, const float *input2, int samples);
static int find_period_fast (struct stretch_cnxt *cnxt, const float *samples);
static int find_period (struct stretch_cnxt *cnxt, const float *samples);

/*
 * Initialize a context of the time stretching code. The shortest and longest periods
 * are specified here. The longest period determines the lowest fundamental frequency
 * that can be handled correctly. Note that higher frequencies can be handled than the
 * shortest period would suggest because multiple periods can be combined, and the
 * worst-case performance will suffer if too short a period is selected. The flags are:
 *
 * STRETCH_FAST_FLAG    0x1     Use the "fast" version of the period calculation
 *
 * STRETCH_DUAL_FLAG    0x2     Cascade two instances of the stretcher to expand
 *                              available ratios to 0.25X to 4.00X
 */

StretchHandle stretch_init (int shortest_period, int longest_period, int num_channels, int flags)
{
    struct stretch_cnxt *cnxt;
    int max_periods = 3;

    if (flags & STRETCH_FAST_FLAG) {
        longest_period = (longest_period + 1) & ~1;
        shortest_period &= ~1;
        max_periods = 4;
    }

    if (longest_period <= shortest_period || shortest_period < MIN_PERIOD || longest_period > MAX_PERIOD) {
        fprintf (stderr, "stretch_init(): invalid periods!\n");
        return NULL;
    }

    cnxt = (struct stretch_cnxt *) calloc (1, sizeof (struct stretch_cnxt));

    if (cnxt) {
        cnxt->inbuff_samples = longest_period * num_channels * max_periods;
        cnxt->inbuff = calloc (cnxt->inbuff_samples, sizeof (*cnxt->inbuff));

        if (num_channels == 2 || (flags & STRETCH_FAST_FLAG))
            cnxt->calcbuff = calloc (longest_period * num_channels, sizeof (*cnxt->calcbuff));

        if ((flags & STRETCH_FAST_FLAG))
            cnxt->results = calloc (longest_period, sizeof (*cnxt->results));
    }

    if (!cnxt || !cnxt->inbuff || (num_channels == 2 && (flags & STRETCH_FAST_FLAG) && !cnxt->calcbuff) || ((flags & STRETCH_FAST_FLAG) && !cnxt->results)) {
        fprintf (stderr, "stretch_init(): out of memory!\n");
        return NULL;
    }

    cnxt->head = cnxt->tail = cnxt->longest = longest_period * num_channels;
    cnxt->fast_mode = (flags & STRETCH_FAST_FLAG) ? 1 : 0;
    cnxt->shortest = shortest_period * num_channels;
    cnxt->num_chans = num_channels;

    if (flags & STRETCH_DUAL_FLAG) {
        cnxt->next = stretch_init (shortest_period, longest_period, num_channels, flags & ~STRETCH_DUAL_FLAG);
        cnxt->intermediate = calloc (longest_period * num_channels * max_periods, sizeof (*cnxt->intermediate));
    }

    return (StretchHandle) cnxt;
}

/*
 * Re-Initialize a context of the time stretching code - as if freshly created
 * with stretch_init(). This drops all internal state.
 */

void stretch_reset (StretchHandle handle)
{
    struct stretch_cnxt *cnxt = (struct stretch_cnxt *) handle;

    cnxt->head = cnxt->tail = cnxt->longest;
    memset (cnxt->inbuff, 0, cnxt->tail * sizeof (*cnxt->inbuff));

    if (cnxt->next)
        stretch_reset (cnxt->next);
}

/*
 * Determine how many samples (per channel) should be reserved in 'output'-array
 * for stretch_samples() and stretch_flush(). max_num_samples and max_ratio are the
 * maximum values that will be passed to stretch_samples().
 */

int stretch_output_capacity (StretchHandle handle, int max_num_samples, float max_ratio)
{
    struct stretch_cnxt *cnxt = (struct stretch_cnxt *) handle;
    int max_period = cnxt->longest / cnxt->num_chans;
    int max_expected_samples;
    float next_ratio;

    if (cnxt->next) {
        if (max_ratio < 0.5) {
            next_ratio = max_ratio / 0.5;
            max_ratio = 0.5;
        }
        else if (max_ratio > 2.0) {
            next_ratio = max_ratio / 2.0;
            max_ratio = 2.0;
        }
        else
            next_ratio = 1.0;
    }

    max_expected_samples = (int) ceil (max_num_samples * ceil (max_ratio * 2.0) / 2.0) +
        max_period * (cnxt->fast_mode ? 4 : 3);

    if (cnxt->next)
        max_expected_samples = stretch_output_capacity (cnxt->next, max_expected_samples, next_ratio);

    return max_expected_samples;
}

/*
 * Process the specified samples with the given ratio (which is normally clipped to
 * the range 0.5 to 2.0, or 0.25 to 4.00 for the "dual" mode). Note that in stereo
 * the number of samples refers to the samples for one channel (i.e., not the total
 * number of values passed) and can be as large as desired (samples are buffered here).
 * The ratio may change between calls, but there is some latency to consider because
 * audio is buffered here and a new ratio may be applied to previously sent samples.
 *
 * The exact number of samples output is not easy to determine in advance, so a function
 * is provided (stretch_output_capacity()) that calculates the maximum number of samples
 * that can be generated from a single call to this function (or stretch_flush()) given
 * a number of samples and maximum ratio. It is recommended that that function be used
 * after initialization to allocate in advance the buffer size required. Be sure to
 * multiply the return value by the number channels!
 *
 * Samples are expected to be normalized float32 values in the range [-1.0, 1.0].
 */

int stretch_samples_float (StretchHandle handle, const float *samples, int num_samples, float *output, float ratio)
{
    struct stretch_cnxt *cnxt = (struct stretch_cnxt *) handle;
    int out_samples = 0, next_samples = 0;
    float *outbuf = output;
    float next_ratio;

    /* if there's a cascaded instance after this one, try to do as much of the ratio here and the rest in "next" */

    if (cnxt->next) {
        outbuf = cnxt->intermediate;

        if (ratio < 0.5) {
            next_ratio = ratio / 0.5;
            ratio = 0.5;
        }
        else if (ratio > 2.0) {
            next_ratio = ratio / 2.0;
            ratio = 2.0;
        }
        else
            next_ratio = 1.0;
    }

    num_samples *= cnxt->num_chans;

    /* this really should not happen, but a good idea to clamp in case */

    if (ratio < 0.5)
        ratio = 0.5;
    else if (ratio > 2.0)
        ratio = 2.0;

    /* while we have pending samples to read into our buffer */

    while (num_samples) {

        /* copy in as many samples as we have room for */

        int samples_to_copy = num_samples;

        if (samples_to_copy > cnxt->inbuff_samples - cnxt->head)
            samples_to_copy = cnxt->inbuff_samples - cnxt->head;

        memcpy (cnxt->inbuff + cnxt->head, samples, samples_to_copy * sizeof (cnxt->inbuff [0]));
        num_samples -= samples_to_copy;
        samples += samples_to_copy;
        cnxt->head += samples_to_copy;

        /* while there are enough samples to process (3 or 4 times the longest period), do so */

        while (cnxt->tail >= cnxt->longest && cnxt->head - cnxt->tail >= cnxt->longest * (cnxt->fast_mode ? 3 : 2)) {
            float process_ratio;
            int period;

            if (ratio != 1.0 || cnxt->outsamples_error)
                period = cnxt->fast_mode ? find_period_fast (cnxt, cnxt->inbuff + cnxt->tail) :
                    find_period (cnxt, cnxt->inbuff + cnxt->tail);
            else
                period = cnxt->longest;

            /*
             * Once we have calculated the best-match period, there are 4 possible transformations
             * available to convert the input samples to output samples. Obviously we can simply
             * copy the samples verbatim (1:1). Standard TDHS provides algorithms for 2:1 and
             * 1:2 scaling, and I have created an obvious extension for 2:3 scaling. To achieve
             * intermediate ratios we maintain a "error" term (in samples) and use that here to
             * calculate the actual transformation to apply.
             */

            if (cnxt->outsamples_error == 0.0)
                process_ratio = floor (ratio * 2.0 + 0.5) / 2.0;
            else if (cnxt->outsamples_error > 0.0)
                process_ratio = floor (ratio * 2.0) / 2.0;
            else
                process_ratio = ceil (ratio * 2.0) / 2.0;

            if (process_ratio == 0.5) {
                merge_blocks_float (outbuf + out_samples, cnxt->inbuff + cnxt->tail,
                    cnxt->inbuff + cnxt->tail + period, period);
                cnxt->outsamples_error += period - (period * 2.0 * ratio);
                out_samples += period;
                cnxt->tail += period * 2;
            }
            else if (process_ratio == 1.0) {
                memcpy (outbuf + out_samples, cnxt->inbuff + cnxt->tail, period * 2 * sizeof (cnxt->inbuff [0]));

                if (ratio != 1.0)
                    cnxt->outsamples_error += (period * 2.0) - (period * 2.0 * ratio);
                else
                    cnxt->outsamples_error = 0; /* if the ratio is 1.0, we can never cancel the error, so just do it now */

                out_samples += period * 2;
                cnxt->tail += period * 2;
            }
            else if (process_ratio == 1.5) {
                memcpy (outbuf + out_samples, cnxt->inbuff + cnxt->tail, period * sizeof (cnxt->inbuff [0]));
                merge_blocks_float (outbuf + out_samples + period, cnxt->inbuff + cnxt->tail + period,
                    cnxt->inbuff + cnxt->tail, period);
                memcpy (outbuf + out_samples + period * 2, cnxt->inbuff + cnxt->tail + period, period * sizeof (cnxt->inbuff [0]));
                cnxt->outsamples_error += (period * 3.0) - (period * 2.0 * ratio);
                out_samples += period * 3;
                cnxt->tail += period * 2;
            }
            else if (process_ratio == 2.0) {
                merge_blocks_float (outbuf + out_samples, cnxt->inbuff + cnxt->tail,
                    cnxt->inbuff + cnxt->tail - period, period * 2);

                cnxt->outsamples_error += (period * 2.0) - (period * ratio);
                out_samples += period * 2;
                cnxt->tail += period;

                if (cnxt->fast_mode) {
                    merge_blocks_float (outbuf + out_samples, cnxt->inbuff + cnxt->tail,
                        cnxt->inbuff + cnxt->tail - period, period * 2);

                    cnxt->outsamples_error += (period * 2.0) - (period * ratio);
                    out_samples += period * 2;
                    cnxt->tail += period;
                }
            }
            else
                fprintf (stderr, "stretch_samples: fatal programming error: process_ratio == %g\n", process_ratio);

            /* if there's another cascaded instance after this, pass the just stretched samples into that */

            if (cnxt->next) {
                next_samples += stretch_samples_float (cnxt->next, outbuf, out_samples / cnxt->num_chans, output + next_samples * cnxt->num_chans, next_ratio);
                out_samples = 0;
            }

            /* finally, left-justify the samples in the buffer leaving one longest period of history */

            int samples_to_move = cnxt->inbuff_samples - cnxt->tail + cnxt->longest;

            memmove (cnxt->inbuff, cnxt->inbuff + cnxt->tail - cnxt->longest,
                samples_to_move * sizeof (cnxt->inbuff [0]));

            cnxt->head -= cnxt->tail - cnxt->longest;
            cnxt->tail = cnxt->longest;
        }
    }

    /*
     * This code is not strictly required, but will reduce latency, especially in the dual-instance case, by
     * always flushing all pending samples if no actual stretching is desired (i.e., ratio is 1.0 and there's
     * no error to compensate for). This case is more common now than previously because of the gap detection
     * and cascaded instances.
     */

    if (ratio == 1.0 && !cnxt->outsamples_error && cnxt->head != cnxt->tail) {
        int samples_leftover = cnxt->head - cnxt->tail;

        if (cnxt->next)
            next_samples += stretch_samples_float (cnxt->next, cnxt->inbuff + cnxt->tail, samples_leftover / cnxt->num_chans,
                output + next_samples * cnxt->num_chans, next_ratio);
        else {
            memcpy (outbuf + out_samples, cnxt->inbuff + cnxt->tail, samples_leftover * sizeof (*output));
            out_samples += samples_leftover;
        }

        memmove (cnxt->inbuff, cnxt->inbuff + cnxt->head - cnxt->longest, cnxt->longest * sizeof (cnxt->inbuff [0]));
        cnxt->head = cnxt->tail = cnxt->longest;
    }

    return cnxt->next ? next_samples : out_samples / cnxt->num_chans;
}

/*
 * int16 compatibility wrapper for stretch_samples_float().
 * Converts int16 samples to float, processes, and converts back.
 */

int stretch_samples (StretchHandle handle, const int16_t *samples, int num_samples, int16_t *output, float ratio)
{
    struct stretch_cnxt *cnxt = (struct stretch_cnxt *) handle;
    int num_chans = cnxt->num_chans;
    int total = num_samples * num_chans;
    int i, result;
    float *float_in, *float_out;
    int max_out;

    float_in = malloc (total * sizeof (float));
    if (!float_in) return 0;

    for (i = 0; i < total; i++)
        float_in[i] = samples[i] * (1.0f / 32768.0f);

    max_out = stretch_output_capacity (handle, num_samples, ratio);
    float_out = malloc (max_out * num_chans * sizeof (float));
    if (!float_out) {
        free (float_in);
        return 0;
    }

    result = stretch_samples_float (handle, float_in, num_samples, float_out, ratio);

    for (i = 0; i < result * num_chans; i++) {
        float v = float_out[i];
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        output[i] = (int16_t)(v * 32767.0f);
    }

    free (float_in);
    free (float_out);
    return result;
}

/*
 * Flush any leftover samples out at normal speed. For cascaded dual instances this must be called
 * twice to completely flush, or simply call it until it returns zero samples. The maximum number
 * of samples that can be returned from each call of this function can be determined in advance with
 * stretch_output_capacity().
 */

int stretch_flush_float (StretchHandle handle, float *output)
{
    struct stretch_cnxt *cnxt = (struct stretch_cnxt *) handle;
    int samples_leftover = cnxt->head - cnxt->tail;
    int samples_flushed = 0;

    if (cnxt->next) {
        if (samples_leftover)
            samples_flushed = stretch_samples_float (cnxt->next, cnxt->inbuff + cnxt->tail, samples_leftover / cnxt->num_chans, output, 1.0);

        if (!samples_flushed)
            samples_flushed = stretch_flush_float (cnxt->next, output);
    }
    else {
        memcpy (output, cnxt->inbuff + cnxt->tail, samples_leftover * sizeof (*output));
        samples_flushed = samples_leftover / cnxt->num_chans;
    }

    cnxt->tail = cnxt->head;
    memset (cnxt->inbuff, 0, cnxt->tail * sizeof (*cnxt->inbuff));

    return samples_flushed;
}

/*
 * int16 compatibility wrapper for stretch_flush_float().
 */

int stretch_flush (StretchHandle handle, int16_t *output)
{
    struct stretch_cnxt *cnxt = (struct stretch_cnxt *) handle;
    int max_out = stretch_output_capacity (handle, cnxt->inbuff_samples / cnxt->num_chans, 1.0);
    float *float_out;
    int result, i;

    float_out = malloc (max_out * cnxt->num_chans * sizeof (float));
    if (!float_out) return 0;

    result = stretch_flush_float (handle, float_out);

    for (i = 0; i < result * cnxt->num_chans; i++) {
        float v = float_out[i];
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        output[i] = (int16_t)(v * 32767.0f);
    }

    free (float_out);
    return result;
}

/* free handle */

void stretch_deinit (StretchHandle handle)
{
    struct stretch_cnxt *cnxt = (struct stretch_cnxt *) handle;

    free (cnxt->calcbuff);
    free (cnxt->results);
    free (cnxt->inbuff);

    if (cnxt->next) {
        stretch_deinit (cnxt->next);
        free (cnxt->intermediate);
    }

    free (cnxt);
}

/*
 * The pitch detection is done by finding the period that produces the
 * maximum value for the following correlation formula applied to two
 * consecutive blocks of the given period length:
 *
 *         sum of the absolute values of each sample in both blocks
 *   ---------------------------------------------------------------------
 *   sum of the absolute differences of each corresponding pair of samples
 *
 * This formula was chosen for two reasons.  First, it produces output values
 * that can directly compared regardless of the pitch period.  Second, the
 * numerator can be accumulated for successive periods, and only the
 * denominator need be completely recalculated.
 *
 * With float32 samples, no scaling or offset is required.
 */

static int find_period (struct stretch_cnxt *cnxt, const float *samples)
{
    float sum, diff, best_factor = 0.0f;
    float *calcbuff = (float *)samples;
    int period, best_period;
    int i, j;

    period = best_period = cnxt->shortest / cnxt->num_chans;

    // convert stereo to mono, and accumulate sum for longest period

    if (cnxt->num_chans == 2) {
        calcbuff = cnxt->calcbuff;

        for (sum = 0.0f, i = j = 0; i < cnxt->longest * 2; i += 2)
            sum += fabsf (calcbuff [j++] = (samples [i] + samples [i+1]) * 0.5f);
    }
    else
        for (sum = 0.0f, i = 0; i < cnxt->longest; ++i)
            sum += fabsf (calcbuff [i]) + fabsf (calcbuff [i+cnxt->longest]);

    // if silence return longest period

    if (sum < 1e-12f)
        return cnxt->longest;

    /* accumulate sum for shortest period size */

    for (sum = 0.0f, i = 0; i < period; ++i)
        sum += fabsf (calcbuff [i]) + fabsf (calcbuff [i+period]);

    /* this loop actually cycles through all period lengths */

    while (1) {
        const float *comp = calcbuff + period * 2;
        const float *ref = calcbuff + period;

        /* compute sum of absolute differences */

        diff = 0.0f;

        while (ref != calcbuff)
            diff += fabsf (*--ref - *--comp);

        /*
         * Here we calculate the correlation factor.
         * With float32, no scaling is needed — just divide directly.
         * Guard against division by zero (perfect match).
         */

        float factor = (diff > 1e-12f) ? (sum / diff) : 1e12f;

        if (factor >= best_factor) {
            best_factor = factor;
            best_period = period;
        }

        /* see if we're done */

        if (period * cnxt->num_chans == cnxt->longest)
            break;

        /* update accumulating sum and current period */

        sum += fabsf (calcbuff [period * 2]) + fabsf (calcbuff [period * 2 + 1]);
        period++;
    }

    return best_period * cnxt->num_chans;
}

/*
 * This pitch detection function is similar to find_period() above, except that it
 * is optimized for speed. The audio data corresponding to two maximum periods is
 * averaged 2:1 into the calculation buffer, and then the calculations are done
 * for every other period length. Because the time is essentially proportional to
 * both the number of samples and the number of period lengths to try, this scheme
 * can reduce the time by a factor approaching 4x. The correlation results on either
 * side of the peak are compared to calculate a more accurate center of the period.
 */

static int find_period_fast (struct stretch_cnxt *cnxt, const float *samples)
{
    float sum, diff, best_factor = 0.0f;
    int period, best_period;
    int i, j;

    best_period = period = cnxt->shortest / (cnxt->num_chans * 2);

    /* first step is compressing data 2:1 into calcbuff, and calculating maximum sum */

    if (cnxt->num_chans == 2)
        for (sum = 0.0f, i = j = 0; i < cnxt->longest * 2; i += 4)
            sum += fabsf (cnxt->calcbuff [j++] =
                (samples [i] + samples [i+1] + samples [i+2] + samples [i+3]) * 0.25f);
    else
        for (sum = 0.0f, i = j = 0; i < cnxt->longest * 2; i += 2)
            sum += fabsf (cnxt->calcbuff [j++] =
                (samples [i] + samples [i+1]) * 0.5f);

    // if silence return longest period

    if (sum < 1e-12f)
        return cnxt->longest;

    /* accumulate sum for shortest period */

    for (sum = 0.0f, i = 0; i < period; ++i)
        sum += fabsf (cnxt->calcbuff [i]) + fabsf (cnxt->calcbuff [i+period]);

    /* this loop actually cycles through all period lengths */

    while (1) {
        const float *comp = cnxt->calcbuff + period * 2;
        const float *ref = cnxt->calcbuff + period;

        /* compute sum of absolute differences */

        diff = 0.0f;

        while (ref != cnxt->calcbuff)
            diff += fabsf (*--ref - *--comp);

        /*
         * Here we calculate and store the resulting correlation
         * factor. With float32, no scaling is needed.
         */

        cnxt->results [period] = (diff > 1e-12f) ? (sum / diff) : 1e12f;

        if (cnxt->results [period] >= best_factor) {    /* check if best yet */
            best_factor = cnxt->results [period];
            best_period = period;
        }

        /* see if we're done */

        if (period * cnxt->num_chans * 2 == cnxt->longest)
            break;

        /* update accumulating sum and current period */

        sum += fabsf (cnxt->calcbuff [period * 2]) + fabsf (cnxt->calcbuff [period * 2 + 1]);
        period++;
    }

    if (best_period * cnxt->num_chans * 2 != cnxt->shortest && best_period * cnxt->num_chans * 2 != cnxt->longest) {
        float high_side_diff = cnxt->results [best_period] - cnxt->results [best_period+1];
        float low_side_diff = cnxt->results [best_period] - cnxt->results [best_period-1];

        if ((low_side_diff + 1.0f) / 2.0f > high_side_diff)
            best_period = best_period * 2 + 1;
        else if ((high_side_diff + 1.0f) / 2.0f > low_side_diff)
            best_period = best_period * 2 - 1;
        else
            best_period *= 2;
    }
    else
        best_period *= 2;           /* shortest or longest use as is */

    return best_period * cnxt->num_chans;
}

/*
 * To combine the two periods into one, each corresponding pair of samples
 * are averaged with a linearly sliding scale.  At the beginning of the period
 * the first sample dominates, and at the end the second sample dominates.  In
 * this way the resulting block blends with the previous and next blocks.
 *
 * With float32 samples, this is a straightforward linear crossfade — no offset
 * or integer rounding tricks are needed. The float arithmetic naturally handles
 * the full dynamic range without compression around zero.
 */

static void merge_blocks_float (float *output, const float *input1, const float *input2, int samples)
{
    int i;
    float inv_samples = 1.0f / (float)samples;

    for (i = 0; i < samples; ++i) {
        float w2 = (float)i * inv_samples;
        float w1 = 1.0f - w2;
        output[i] = input1[i] * w1 + input2[i] * w2;
    }
}
