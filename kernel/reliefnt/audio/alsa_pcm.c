#include <limits.h>
#include <stddef.h>
#include <string.h>

#include <linux/errno.h>
#include <reliefnt/audio.h>
#include <sound/asound.h>

struct audio_alsa_format {
    uint64_t capability;
    unsigned int number;
    unsigned int sample_bits;
};

struct audio_alsa_rate {
    uint64_t capability;
    unsigned int rate;
};

static const struct audio_alsa_format audio_alsa_formats[] = {
    {AUDIO_FORMAT_S8, SNDRV_PCM_FORMAT_S8, 8},
    {AUDIO_FORMAT_S16_LE, SNDRV_PCM_FORMAT_S16_LE, 16},
    {AUDIO_FORMAT_S20_LE, SNDRV_PCM_FORMAT_S20_LE, 32},
    {AUDIO_FORMAT_S24_LE, SNDRV_PCM_FORMAT_S24_LE, 32},
    {AUDIO_FORMAT_S32_LE, SNDRV_PCM_FORMAT_S32_LE, 32},
    {AUDIO_FORMAT_U8, SNDRV_PCM_FORMAT_U8, 8},
};

static const struct audio_alsa_rate audio_alsa_rates[] = {
    {AUDIO_RATE_8000, 8000},
    {AUDIO_RATE_11025, 11025},
    {AUDIO_RATE_16000, 16000},
    {AUDIO_RATE_22050, 22050},
    {AUDIO_RATE_32000, 32000},
    {AUDIO_RATE_44100, 44100},
    {AUDIO_RATE_48000, 48000},
    {AUDIO_RATE_88200, 88200},
    {AUDIO_RATE_96000, 96000},
    {AUDIO_RATE_176400, 176400},
    {AUDIO_RATE_192000, 192000},
};

/** @brief Return an ALSA interval by its public parameter number. */
static struct snd_interval *audio_alsa_interval(struct snd_pcm_hw_params *params,
                                                 unsigned int parameter)
{
    return &params->intervals[parameter - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
}

/** @brief Test whether an ALSA mask contains one numbered value. */
static int audio_alsa_mask_has(const struct snd_mask *mask, unsigned int value)
{
    return value < SNDRV_MASK_MAX &&
           (mask->bits[value / 32u] & (1u << (value % 32u))) != 0;
}

/** @brief Return whether an ALSA mask retains at least one candidate. */
static int audio_alsa_mask_any(const struct snd_mask *mask)
{
    for (size_t word = 0; word < sizeof(mask->bits) / sizeof(mask->bits[0]); ++word)
        if (mask->bits[word] != 0) return 1;
    return 0;
}

/** @brief Replace an ALSA mask with the values supported by a hardware mask. */
static int audio_alsa_mask_formats(struct snd_mask *mask, uint64_t capabilities)
{
    struct snd_mask original = *mask;
    memset(mask, 0, sizeof(*mask));
    for (size_t i = 0; i < sizeof(audio_alsa_formats) / sizeof(audio_alsa_formats[0]); ++i)
        if ((capabilities & audio_alsa_formats[i].capability) != 0)
            mask->bits[audio_alsa_formats[i].number / 32u] |=
                1u << (audio_alsa_formats[i].number % 32u);
    for (size_t word = 0; word < sizeof(mask->bits) / sizeof(mask->bits[0]); ++word)
        mask->bits[word] &= original.bits[word];
    return memcmp(&original, mask, sizeof(original)) != 0;
}

/** @brief Replace the access mask with the two supported interleaved modes. */
static int audio_alsa_mask_access(struct snd_mask *mask)
{
    struct snd_mask original = *mask;
    memset(mask, 0, sizeof(*mask));
    mask->bits[SNDRV_PCM_ACCESS_MMAP_INTERLEAVED / 32u] |=
        1u << (SNDRV_PCM_ACCESS_MMAP_INTERLEAVED % 32u);
    mask->bits[SNDRV_PCM_ACCESS_RW_INTERLEAVED / 32u] |=
        1u << (SNDRV_PCM_ACCESS_RW_INTERLEAVED % 32u);
    for (size_t word = 0; word < sizeof(mask->bits) / sizeof(mask->bits[0]); ++word)
        mask->bits[word] &= original.bits[word];
    return memcmp(&original, mask, sizeof(original)) != 0;
}

/** @brief Propagate actual per-format subformats in both directions.
 * @param params Caller-owned hardware constraint masks.
 * @param caps Borrowed hardware format/subformat capability table.
 * @return Zero or -EINVAL for an empty pair. Task context; no ownership transfer.
 */
static int audio_alsa_subformats(struct snd_pcm_hw_params *params,
                                 const struct audio_format_caps *caps)
{
    struct snd_mask *formats = &params->masks[SNDRV_PCM_HW_PARAM_FORMAT];
    struct snd_mask *subs = &params->masks[SNDRV_PCM_HW_PARAM_SUBFORMAT];
    uint32_t candidates = 0;
    for (size_t i = 0; i < sizeof(audio_alsa_formats)/sizeof(audio_alsa_formats[0]); ++i) {
        const struct audio_alsa_format *format = &audio_alsa_formats[i];
        if (!audio_alsa_mask_has(formats, format->number)) continue;
        uint32_t supported = 0;
        for (unsigned n = 0; n < 6; ++n)
            if (caps->format_bits[n] == format->capability) supported |= caps->subformats[n];
        uint32_t linux_subs = 0;
        if (supported & (1u << AUDIO_SUBFORMAT_STD)) linux_subs |= 1u << SNDRV_PCM_SUBFORMAT_STD;
        if (supported & (1u << AUDIO_SUBFORMAT_MSBITS_MAX)) linux_subs |= 1u << SNDRV_PCM_SUBFORMAT_MSBITS_MAX;
        if (supported & (1u << AUDIO_SUBFORMAT_MSBITS_20)) linux_subs |= 1u << SNDRV_PCM_SUBFORMAT_MSBITS_20;
        if (supported & (1u << AUDIO_SUBFORMAT_MSBITS_24)) linux_subs |= 1u << SNDRV_PCM_SUBFORMAT_MSBITS_24;
        supported = linux_subs & subs->bits[0];
        if (!supported) formats->bits[format->number/32u] &= ~(1u << (format->number%32u));
        candidates |= supported;
    }
    memset(subs, 0, sizeof(*subs));
    subs->bits[0] = candidates;
    return candidates && audio_alsa_mask_any(formats) ? 0 : -EINVAL;
}

/** @brief Intersect an integer interval without admitting excluded endpoints.
 * @param interval Caller-owned mutable interval, required.
 * @param minimum Inclusive 64-bit lower limit.
 * @param maximum Inclusive 64-bit upper limit, saturated at UINT_MAX.
 * @return Zero, or -EINVAL for an empty/overflowing intersection. Task context.
 */
static int audio_alsa_intersect(struct snd_interval *interval,
                                uint64_t minimum, uint64_t maximum)
{
    if (interval->empty || minimum > UINT_MAX || minimum > maximum) return -EINVAL;
    uint64_t lo = (uint64_t)interval->min + interval->openmin;
    uint64_t hi = interval->max;
    if (interval->openmax) {
        if (!hi) return -EINVAL;
        --hi;
    }
    if (lo < minimum) lo = minimum;
    if (hi > maximum) hi = maximum;
    if (hi > UINT_MAX) hi = UINT_MAX;
    if (lo > hi) { interval->empty = 1; return -EINVAL; }
    interval->min = (unsigned int)lo;
    interval->max = (unsigned int)hi;
    interval->openmin = interval->openmax = 0;
    interval->integer = 1;
    return 0;
}

/** @brief Intersect a microsecond interval while retaining fractional bounds.
 * @param interval Caller-owned time interval; integer requests remain integer.
 * @param minimum Rounded-down inclusive or open lower bound, in microseconds.
 * @param maximum Rounded-up inclusive or open upper bound, in microseconds.
 * @param openmin Whether the lower bound excludes its rounded endpoint.
 * @param openmax Whether the upper bound excludes its rounded endpoint.
 * @return Zero or -EINVAL for an empty intersection. Task context; bounds shrink.
 */
static int audio_alsa_time_intersect(struct snd_interval *interval,
                                     uint64_t minimum, uint64_t maximum,
                                     unsigned int openmin, unsigned int openmax)
{
    if (interval->empty || minimum > UINT_MAX || minimum > maximum) return -EINVAL;
    if (maximum > UINT_MAX) { maximum = UINT_MAX; openmax = 0; }
    if (interval->min < minimum) {
        interval->min = (unsigned int)minimum;
        interval->openmin = openmin;
    } else if (interval->min == minimum) interval->openmin |= openmin;
    if (interval->max > maximum) {
        interval->max = (unsigned int)maximum;
        interval->openmax = openmax;
    } else if (interval->max == maximum) interval->openmax |= openmax;
    if (interval->integer) {
        if ((interval->openmin && interval->min == UINT_MAX) ||
            (interval->openmax && !interval->max)) {
            interval->empty = 1;
            return -EINVAL;
        }
        interval->min += interval->openmin;
        interval->max -= interval->openmax;
        interval->openmin = interval->openmax = 0;
    } else if (interval->min == interval->max &&
               !interval->openmin && !interval->openmax) interval->integer = 1;
    if (interval->min > interval->max ||
        (interval->min == interval->max && (interval->openmin || interval->openmax))) {
        interval->empty = 1;
        return -EINVAL;
    }
    return 0;
}

/** @brief Remove formats inconsistent with the selected physical sample width.
 * @param params Caller-owned hardware constraints.
 * @return Zero or -EINVAL for an empty selection. Task context, no ownership transfer.
 */
static int audio_alsa_format_refine(struct snd_pcm_hw_params *params)
{
    struct snd_mask *mask = &params->masks[SNDRV_PCM_HW_PARAM_FORMAT];
    struct snd_interval *bits = audio_alsa_interval(params, SNDRV_PCM_HW_PARAM_SAMPLE_BITS);
    unsigned int minimum = UINT_MAX, maximum = 0;
    for (size_t i=0; i<sizeof(audio_alsa_formats)/sizeof(audio_alsa_formats[0]); ++i) {
        const struct audio_alsa_format *f = &audio_alsa_formats[i];
        if (!audio_alsa_mask_has(mask,f->number)) continue;
        if (f->sample_bits < bits->min || f->sample_bits > bits->max) {
            mask->bits[f->number/32u] &= ~(1u << (f->number%32u));
            continue;
        }
        if (minimum > f->sample_bits) minimum = f->sample_bits;
        if (maximum < f->sample_bits) maximum = f->sample_bits;
    }
    return maximum ? audio_alsa_intersect(bits,minimum,maximum) : -EINVAL;
}

/** @brief Restrict a rate interval to actual discrete hardware candidates.
 * @param capabilities Driver rate mask.
 * @param interval Caller-owned normalized integer rate interval.
 * @return Zero or -EINVAL when no supported candidate remains. Task context.
 */
static int audio_alsa_rate_refine(uint64_t capabilities, struct snd_interval *interval)
{
    unsigned int minimum = UINT_MAX, maximum = 0;
    for (size_t i=0; i<sizeof(audio_alsa_rates)/sizeof(audio_alsa_rates[0]); ++i) {
        unsigned int rate = audio_alsa_rates[i].rate;
        if (!(capabilities & audio_alsa_rates[i].capability) ||
            rate < interval->min || rate > interval->max) continue;
        if (minimum > rate) minimum = rate;
        if (maximum < rate) maximum = rate;
    }
    return maximum ? audio_alsa_intersect(interval,minimum,maximum) : -EINVAL;
}

/** @brief Propagate an integer product relation in both directions.
 * @param params Caller-owned normalized hardware intervals.
 * @param a First positive integer factor parameter.
 * @param b Second positive integer factor parameter.
 * @param c Product parameter, equal to a*b/divisor.
 * @param divisor Positive exact unit conversion (one or eight).
 * @return Zero or -EINVAL for contradictions; 64-bit products cannot overflow
 * for 32-bit factors. Task context; bounds only shrink.
 */
static int audio_alsa_product(struct snd_pcm_hw_params *params,
                              unsigned int a, unsigned int b, unsigned int c,
                              unsigned int divisor)
{
    struct snd_interval *x = audio_alsa_interval(params,a);
    struct snd_interval *y = audio_alsa_interval(params,b);
    struct snd_interval *z = audio_alsa_interval(params,c);
    uint64_t lo = (uint64_t)x->min*y->min;
    uint64_t hi = (uint64_t)x->max*y->max;
    if (audio_alsa_intersect(z,(lo+divisor-1u)/divisor,hi/divisor)) return -EINVAL;
    lo = (uint64_t)z->min*divisor;
    hi = (uint64_t)z->max*divisor;
    if (audio_alsa_intersect(x,(lo+y->max-1u)/y->max,hi/y->min) ||
        audio_alsa_intersect(y,(lo+x->max-1u)/x->max,hi/x->min)) return -EINVAL;
    return 0;
}

/** @brief Propagate the ALSA frame/rate to microsecond time relation.
 * @param params Caller-owned normalized hardware intervals.
 * @param frames_param Period or buffer frames parameter number.
 * @param time_param Matching period or buffer time parameter number.
 * @return Zero, or -EINVAL for an empty relation. Task context.
 *
 * Linux exposes period_time and buffer_time as first-class constraints even
 * though the core stores frame geometry. Keep both views synchronized so a
 * userspace near-selection cannot install an impossible time/size pair.
 */
static int audio_alsa_time_relation(struct snd_pcm_hw_params *params,
                                    unsigned int frames_param,
                                    unsigned int time_param)
{
    struct snd_interval *frames = audio_alsa_interval(params, frames_param);
    struct snd_interval *time = audio_alsa_interval(params, time_param);
    const struct snd_interval *rate =
        audio_alsa_interval(params, SNDRV_PCM_HW_PARAM_RATE);
    if (frames->empty || time->empty || rate->empty || !rate->min || !rate->max)
        return -EINVAL;

    uint64_t frames_min = (uint64_t)frames->min * 1000000u;
    uint64_t frames_max = (uint64_t)frames->max * 1000000u;
    uint64_t time_min = frames_min / rate->max;
    uint64_t time_max = frames_max / rate->min;
    if (frames_max % rate->min) ++time_max;
    if (audio_alsa_time_intersect(time, time_min, time_max,
                                  frames_min % rate->max != 0,
                                  frames_max % rate->min != 0)) return -EINVAL;

    uint64_t time_value_min = (uint64_t)time->min * rate->min;
    uint64_t time_value_max = (uint64_t)time->max * rate->max;
    uint64_t derived_frames_min = time_value_min / 1000000u;
    uint64_t derived_frames_max = time_value_max / 1000000u;
    if (time_value_min % 1000000u || time->openmin) ++derived_frames_min;
    if (!(time_value_max % 1000000u) && time->openmax) {
        if (!derived_frames_max) return -EINVAL;
        --derived_frames_max;
    }
    if (derived_frames_max > UINT_MAX) derived_frames_max = UINT_MAX;
    if (audio_alsa_intersect(frames, derived_frames_min, derived_frames_max))
        return -EINVAL;
    return 0;
}

/** @brief Intersect a PCM with its card capabilities using Linux ALSA ranges. */
int audio_alsa_refine(struct audio_pcm *pcm, struct snd_pcm_hw_params *params)
{
    if (!pcm || !params) return -EINVAL;
    struct snd_pcm_hw_params original = *params;
    struct audio_format_caps extended;
    int ret = audio_pcm_format_caps(pcm, &extended);
    if (ret) return ret;
    struct audio_caps caps = extended.pcm;
    (void)audio_alsa_mask_access(&params->masks[SNDRV_PCM_HW_PARAM_ACCESS]);
    (void)audio_alsa_mask_formats(&params->masks[SNDRV_PCM_HW_PARAM_FORMAT], caps.formats);
    if (audio_alsa_subformats(params, &extended)) {
        return -EINVAL;
    }
    if (!audio_alsa_mask_has(&params->masks[SNDRV_PCM_HW_PARAM_ACCESS],
                             SNDRV_PCM_ACCESS_MMAP_INTERLEAVED) &&
        !audio_alsa_mask_has(&params->masks[SNDRV_PCM_HW_PARAM_ACCESS],
                             SNDRV_PCM_ACCESS_RW_INTERLEAVED)) {
        return -EINVAL;
    }
    if (!audio_alsa_mask_any(&params->masks[SNDRV_PCM_HW_PARAM_FORMAT])) {
        return -EINVAL;
    }
    if (!audio_alsa_mask_any(&params->masks[SNDRV_PCM_HW_PARAM_SUBFORMAT])) {
        return -EINVAL;
    }
    for (unsigned int i=0; i<=SNDRV_PCM_HW_PARAM_LAST_INTERVAL-
                                  SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; ++i) {
        unsigned int parameter = i + SNDRV_PCM_HW_PARAM_FIRST_INTERVAL;
        int result = parameter == SNDRV_PCM_HW_PARAM_PERIOD_TIME ||
                     parameter == SNDRV_PCM_HW_PARAM_BUFFER_TIME
            ? audio_alsa_time_intersect(&params->intervals[i], 1, UINT_MAX, 0, 0)
            : audio_alsa_intersect(&params->intervals[i],
                                   parameter == SNDRV_PCM_HW_PARAM_TICK_TIME ? 0 : 1, UINT_MAX);
        if (result) {
            return -EINVAL;
        }
    }
    if (audio_alsa_intersect(audio_alsa_interval(params,SNDRV_PCM_HW_PARAM_CHANNELS),
                             caps.channels_min,caps.channels_max) ||
        audio_alsa_intersect(audio_alsa_interval(params,SNDRV_PCM_HW_PARAM_PERIOD_BYTES),
                             caps.period_bytes_min,caps.period_bytes_max) ||
        audio_alsa_intersect(audio_alsa_interval(params,SNDRV_PCM_HW_PARAM_BUFFER_BYTES),
                             1,caps.buffer_bytes_max) ||
        audio_alsa_intersect(audio_alsa_interval(params,SNDRV_PCM_HW_PARAM_PERIODS),
                             extended.period_count_min ? extended.period_count_min : 1,
                             extended.period_count_max ? extended.period_count_max : UINT_MAX)) {
        return -EINVAL;
    }
    for (unsigned int pass=0;; ++pass) {
        struct snd_pcm_hw_params previous = *params;
        if (audio_alsa_subformats(params, &extended)) {
            return -EINVAL;
        }
        if (audio_alsa_format_refine(params)) {
            return -EINVAL;
        }
        if (audio_alsa_rate_refine(caps.rates,audio_alsa_interval(params,SNDRV_PCM_HW_PARAM_RATE))) {
            return -EINVAL;
        }
        if (audio_alsa_product(params,SNDRV_PCM_HW_PARAM_SAMPLE_BITS,
                               SNDRV_PCM_HW_PARAM_CHANNELS,SNDRV_PCM_HW_PARAM_FRAME_BITS,1) ||
            audio_alsa_product(params,SNDRV_PCM_HW_PARAM_FRAME_BITS,
                               SNDRV_PCM_HW_PARAM_PERIOD_SIZE,SNDRV_PCM_HW_PARAM_PERIOD_BYTES,8) ||
            audio_alsa_product(params,SNDRV_PCM_HW_PARAM_FRAME_BITS,
                               SNDRV_PCM_HW_PARAM_BUFFER_SIZE,SNDRV_PCM_HW_PARAM_BUFFER_BYTES,8) ||
            audio_alsa_product(params,SNDRV_PCM_HW_PARAM_PERIOD_SIZE,
                               SNDRV_PCM_HW_PARAM_PERIODS,SNDRV_PCM_HW_PARAM_BUFFER_SIZE,1)) {
            return -EINVAL;
        }
        if (audio_alsa_time_relation(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE,
                                     SNDRV_PCM_HW_PARAM_PERIOD_TIME) ||
            audio_alsa_time_relation(params, SNDRV_PCM_HW_PARAM_BUFFER_SIZE,
                                     SNDRV_PCM_HW_PARAM_BUFFER_TIME)) {
            return -EINVAL;
        }
        if (!memcmp(previous.masks,params->masks,sizeof(params->masks)) &&
            !memcmp(previous.intervals,params->intervals,sizeof(params->intervals))) break;
        /* Bound hostile refinement requests; reject rather than returning an
         * unstable constraint set. Normal integer geometry converges rapidly. */
        if (pass >= 127u) {
            return -EINVAL;
        }
    }
    params->info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
                   SNDRV_PCM_INFO_INTERLEAVED;
    params->cmask = 0;
    for (unsigned int i = 0;
         i <= SNDRV_PCM_HW_PARAM_LAST_MASK - SNDRV_PCM_HW_PARAM_FIRST_MASK; ++i)
        if (memcmp(&original.masks[i], &params->masks[i], sizeof(params->masks[i])) != 0)
            params->cmask |= 1u << (SNDRV_PCM_HW_PARAM_FIRST_MASK + i);
    for (unsigned int i = 0;
         i <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; ++i)
        if (memcmp(&original.intervals[i], &params->intervals[i],
                   sizeof(params->intervals[i])) != 0)
            params->cmask |= 1u << (SNDRV_PCM_HW_PARAM_FIRST_INTERVAL + i);
    params->rmask = 0;
    return 0;
}

/** @brief Require an interval to select one unsigned integer value. */
static int audio_alsa_single(const struct snd_interval *interval,
                             unsigned int *value)
{
    if (!interval || interval->empty || interval->openmin || interval->openmax ||
        interval->min != interval->max)
        return -EINVAL;
    *value = interval->min;
    return 0;
}

/** @brief Translate one selected Linux format into the core format contract. */
static int audio_alsa_selected_format(const struct snd_mask *mask,
                                      unsigned int *sample_bits)
{
    for (size_t i = 0; i < sizeof(audio_alsa_formats) / sizeof(audio_alsa_formats[0]); ++i)
        if (audio_alsa_mask_has(mask, audio_alsa_formats[i].number)) {
            *sample_bits = audio_alsa_formats[i].sample_bits;
            return 0;
        }
    return -EINVAL;
}

/** @brief Build and commit one final ALSA hardware parameter selection. */
static int audio_alsa_hw_params(struct audio_pcm *pcm,
                                struct snd_pcm_hw_params *params)
{
    struct snd_pcm_hw_params refined = *params;
    int ret = audio_alsa_refine(pcm, &refined);
    if (ret) return ret;
    unsigned int rate, channels, sample_bits, frame_bits;
    unsigned int period_frames, buffer_frames;
    if (audio_alsa_single(audio_alsa_interval(&refined, SNDRV_PCM_HW_PARAM_RATE), &rate) ||
        audio_alsa_single(audio_alsa_interval(&refined, SNDRV_PCM_HW_PARAM_CHANNELS),
                          &channels) ||
        audio_alsa_single(audio_alsa_interval(&refined, SNDRV_PCM_HW_PARAM_SAMPLE_BITS),
                          &sample_bits) ||
        audio_alsa_single(audio_alsa_interval(&refined, SNDRV_PCM_HW_PARAM_FRAME_BITS),
                          &frame_bits) ||
        audio_alsa_single(audio_alsa_interval(&refined, SNDRV_PCM_HW_PARAM_PERIOD_SIZE),
                          &period_frames) ||
        audio_alsa_single(audio_alsa_interval(&refined, SNDRV_PCM_HW_PARAM_BUFFER_SIZE),
                          &buffer_frames))
        return -EINVAL;
    unsigned int selected_bits;
    unsigned int format_count = 0, access_count = 0;
    for (unsigned int n=0; n<SNDRV_MASK_MAX; ++n) {
        format_count += audio_alsa_mask_has(&refined.masks[SNDRV_PCM_HW_PARAM_FORMAT],n);
        access_count += audio_alsa_mask_has(&refined.masks[SNDRV_PCM_HW_PARAM_ACCESS],n);
    }
    if (format_count != 1 || access_count != 1 ||
        audio_alsa_selected_format(&refined.masks[SNDRV_PCM_HW_PARAM_FORMAT],
                                   &selected_bits) || selected_bits != sample_bits ||
        !frame_bits || frame_bits % 8u != 0 ||
        frame_bits != sample_bits * (uint64_t)channels)
        return -EINVAL;
    struct audio_params selected = {
        .rate = rate,
        .channels = channels,
        .sample_bits = sample_bits,
        .frame_bytes = frame_bits / 8u,
        .period_frames = period_frames,
        .buffer_frames = buffer_frames,
    };
    unsigned int subformat = 0, subformat_count = 0;
    for (unsigned int n = 0; n < SNDRV_MASK_MAX; ++n)
        if (audio_alsa_mask_has(&refined.masks[SNDRV_PCM_HW_PARAM_SUBFORMAT], n)) {
            subformat = n; ++subformat_count;
        }
    if (subformat_count != 1) return -EINVAL;
    unsigned int core_subformat = subformat == SNDRV_PCM_SUBFORMAT_MSBITS_MAX ? AUDIO_SUBFORMAT_MSBITS_MAX :
        subformat == SNDRV_PCM_SUBFORMAT_MSBITS_20 ? AUDIO_SUBFORMAT_MSBITS_20 :
        subformat == SNDRV_PCM_SUBFORMAT_MSBITS_24 ? AUDIO_SUBFORMAT_MSBITS_24 : AUDIO_SUBFORMAT_STD;
    struct audio_params_ext ext = {.pcm = selected, .subformat = core_subformat,
                                   .significant_bits = sample_bits};
    for (size_t i = 0; i < sizeof(audio_alsa_formats)/sizeof(audio_alsa_formats[0]); ++i)
        if (audio_alsa_mask_has(&refined.masks[SNDRV_PCM_HW_PARAM_FORMAT],
                                audio_alsa_formats[i].number)) {
            ext.format = (uint32_t)audio_alsa_formats[i].capability;
            if (ext.format == AUDIO_FORMAT_S20_LE) ext.significant_bits = ext.pcm.sample_bits = 20;
            if (ext.format == AUDIO_FORMAT_S24_LE) ext.significant_bits = ext.pcm.sample_bits = 24;
        }
    if (core_subformat == AUDIO_SUBFORMAT_MSBITS_20) ext.significant_bits = 20;
    if (core_subformat == AUDIO_SUBFORMAT_MSBITS_24) ext.significant_bits = 24;
    ret = audio_pcm_hw_params_format(pcm, &ext);
    if (ret) return ret;
    *params = refined;
    params->rate_num = rate;
    params->rate_den = 1;
    params->msbits = ext.significant_bits;
    return 0;
}

/** @brief Copy ring counters into Linux status without hiding XRUN availability.
 * @param pcm Borrowed live PCM; snapshot locking is handled by the core.
 * @param status Output Linux structure, zeroed on a valid snapshot.
 * @return Zero or the core device error. Task context, no ownership transfer.
 * Delay is meaningful only while running; a playback deficit stays signed.
 */
static int audio_alsa_status(struct audio_pcm *pcm, struct snd_pcm_status *status)
{
    struct audio_pcm_status snapshot;
    int ret = audio_pcm_status(pcm, &snapshot);
    if (ret) return ret;
    memset(status, 0, sizeof(*status));
    status->state = (snd_pcm_state_t)snapshot.state;
    status->appl_ptr = snapshot.appl_ptr;
    status->hw_ptr = snapshot.hw_ptr;
    status->tstamp.tv_sec = snapshot.tstamp.tv_sec;
    status->tstamp.tv_nsec = snapshot.tstamp.tv_nsec;
    status->trigger_tstamp.tv_sec = snapshot.trigger_tstamp.tv_sec;
    status->trigger_tstamp.tv_nsec = snapshot.trigger_tstamp.tv_nsec;
    uint64_t boundary = snapshot.boundary;
    if (!boundary) return 0;
    uint64_t available = (snapshot.hw_ptr + boundary - snapshot.appl_ptr) % boundary;
    /* Linux exposes availability beyond one ring after XRUN and for direct
     * dmix/dsnoop slaves. Clamping would hide the underrun/overrun from clients. */
    status->avail = snapshot.direction == AUDIO_PLAYBACK
        ? (available + snapshot.buffer_frames) % boundary : available;
    if (snapshot.state == AUDIO_PCM_RUNNING ||
        (snapshot.state == AUDIO_PCM_DRAINING && snapshot.direction == AUDIO_PLAYBACK))
        status->delay = snapshot.direction == AUDIO_PLAYBACK
            ? (snd_pcm_sframes_t)snapshot.buffer_frames - (snd_pcm_sframes_t)status->avail
            : (snd_pcm_sframes_t)available;
    return 0;
}

/** @brief Fill the Linux mmap status portion from a PCM snapshot. */
static void audio_alsa_sync_status(const struct audio_pcm_status *snapshot,
                                   struct __snd_pcm_mmap_status *status)
{
    memset(status, 0, sizeof(*status));
    status->state = (snd_pcm_state_t)snapshot->state;
    status->hw_ptr = snapshot->hw_ptr;
    status->tstamp.tv_sec = snapshot->tstamp.tv_sec;
    status->tstamp.tv_nsec = snapshot->tstamp.tv_nsec;
}

/** @brief Dispatch Linux PCM protocol and state-machine requests. */
int audio_alsa_ioctl(struct audio_pcm *pcm, uint64_t request, void *argument)
{
    if (!pcm) return -EINVAL;
    switch (request) {
    case SNDRV_PCM_IOCTL_INFO:
        return argument ? audio_pcm_info(pcm, argument) : -EFAULT;
    case SNDRV_PCM_IOCTL_SW_PARAMS:
        return argument ? audio_pcm_sw_params(pcm, argument) : -EFAULT;
    case SNDRV_PCM_IOCTL_CHANNEL_INFO:
        return argument ? audio_pcm_channel_info(pcm, argument) : -EFAULT;
    case SNDRV_PCM_IOCTL_UNLINK:
        return audio_pcm_unlink(pcm);
    case SNDRV_PCM_IOCTL_REWIND:
    case SNDRV_PCM_IOCTL_FORWARD: {
        if (!argument) return -EFAULT;
        long moved = audio_pcm_move(pcm, *(snd_pcm_uframes_t *)argument,
                                    request == SNDRV_PCM_IOCTL_FORWARD);
        if (moved < 0) return (int)moved;
        *(snd_pcm_uframes_t *)argument = (snd_pcm_uframes_t)moved;
        return 0;
    }
    case SNDRV_PCM_IOCTL_PVERSION:
        if (!argument) return -EFAULT;
        *(int *)argument = SNDRV_PCM_VERSION;
        return 0;
    case SNDRV_PCM_IOCTL_USER_PVERSION:
        if (!argument) return -EFAULT;
        return 0; /* Linux stores the client version in the OFD adapter. */
    case SNDRV_PCM_IOCTL_TSTAMP:
        return 0; /* Linux v6.14 compatibility no-op. */
    case SNDRV_PCM_IOCTL_TTSTAMP:
        if (!argument || *(int *)argument < SNDRV_PCM_TSTAMP_TYPE_GETTIMEOFDAY ||
            *(int *)argument > SNDRV_PCM_TSTAMP_TYPE_LAST)
            return -EINVAL;
        return audio_pcm_tstamp(pcm, *(int *)argument);
    case SNDRV_PCM_IOCTL_HW_REFINE: {
        return argument ? audio_alsa_refine(pcm, argument) : -EFAULT;
    }
    case SNDRV_PCM_IOCTL_HW_PARAMS:
        return argument ? audio_alsa_hw_params(pcm, argument) : -EFAULT;
    case SNDRV_PCM_IOCTL_HW_FREE:
        return audio_pcm_hw_free(pcm);
    case SNDRV_PCM_IOCTL_RESET:
        return audio_pcm_reset(pcm);
    case SNDRV_PCM_IOCTL_XRUN:
        return audio_pcm_xrun(pcm);
    case SNDRV_PCM_IOCTL_STATUS:
    case SNDRV_PCM_IOCTL_STATUS_EXT:
        return argument ? audio_alsa_status(pcm, argument) : -EFAULT;
    case SNDRV_PCM_IOCTL_DELAY: {
        if (!argument) return -EFAULT;
        int ret = audio_pcm_hwsync(pcm);
        if (ret) return ret;
        struct snd_pcm_status status;
        ret = audio_alsa_status(pcm, &status);
        if (!ret) *(snd_pcm_sframes_t *)argument = status.delay;
        return ret;
    }
    case SNDRV_PCM_IOCTL_HWSYNC:
        return audio_pcm_hwsync(pcm);
    case SNDRV_PCM_IOCTL_SYNC_PTR: {
        if (!argument) return -EFAULT;
        struct snd_pcm_sync_ptr *sync = argument;
        if (sync->flags & ~(SNDRV_PCM_SYNC_PTR_HWSYNC |
                            SNDRV_PCM_SYNC_PTR_APPL |
                            SNDRV_PCM_SYNC_PTR_AVAIL_MIN)) return -EINVAL;
        struct audio_pcm_status snapshot;
        int ret = audio_pcm_sync(pcm, sync->flags,
                                  sync->c.control.appl_ptr,
                                  sync->c.control.avail_min,
                                  &snapshot);
        if (ret) return ret;
        audio_alsa_sync_status(&snapshot, &sync->s.status);
        sync->c.control.appl_ptr = snapshot.appl_ptr;
        sync->c.control.avail_min = snapshot.avail_min;
        return 0;
    }
    case SNDRV_PCM_IOCTL_PREPARE:
        return audio_pcm_prepare(pcm);
    case SNDRV_PCM_IOCTL_START:
        return audio_pcm_start(pcm);
    case SNDRV_PCM_IOCTL_DROP:
        return audio_pcm_drop(pcm);
    case SNDRV_PCM_IOCTL_DRAIN:
        return audio_pcm_drain(pcm);
    case SNDRV_PCM_IOCTL_PAUSE:
        if (!argument) return -EFAULT;
        return audio_pcm_pause(pcm, *(int *)argument != 0);
    case SNDRV_PCM_IOCTL_RESUME:
        return audio_pcm_pause(pcm, false);
    case SNDRV_PCM_IOCTL_WRITEI_FRAMES:
    case SNDRV_PCM_IOCTL_READI_FRAMES: {
        if (!argument) return -EFAULT;
        struct snd_xferi *transfer = argument;
        if (transfer->frames > UINT32_MAX ||
            (transfer->frames && !transfer->buf)) return -EFAULT;
        enum audio_direction direction = request == SNDRV_PCM_IOCTL_WRITEI_FRAMES
            ? AUDIO_PLAYBACK : AUDIO_CAPTURE;
        struct audio_pcm_status status;
        int ret = audio_pcm_status(pcm, &status);
        if (ret) return ret;
        if (status.direction != direction) return -EBADF;
        long frames = audio_pcm_transfer(pcm, transfer->buf, (uint32_t)transfer->frames);
        if (frames < 0) return (int)frames;
        transfer->result = (snd_pcm_sframes_t)frames;
        return 0;
    }
    default:
        return -ENOTTY;
    }
}
