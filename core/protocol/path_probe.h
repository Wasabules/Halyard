/* path_probe - what the link really carries, estimated from the stream itself.
 *
 * === WHY NOT MEASURE IT THE WAY THE OFFICIAL CLIENT DOES ===
 *
 * The official launcher measures the link with traffic of its own. We cannot
 * copy that: no capture contains an entry point of the kind, and this repo does
 * not invent traffic the official client never sends - speaking on a channel the
 * server does not expect can kill it (KB §9, S57).
 *
 * We have better. The stream IS the measurement: it runs the real protocol, over
 * UDP, in bursts, against the real server, on the real path. A speed test tells
 * you what an unrelated TCP transfer achieved; this tells you what THIS session
 * achieved.
 *
 * === WHAT THE SERVER'S OWN BEHAVIOUR GIVES US ===
 *
 * The server regulates its own bitrate: it climbs while it gets through, loses
 * packets, backs off, climbs again. Measured over a console session of 122
 * five-second samples: mean bitrate 16.2 Mb/s in the samples WITH loss against
 * 14.4 without, and loss appearing only above a threshold. The ceiling is
 * therefore between the two, and it reads out without sending anything special.
 *
 * === WHY THREE NUMBERS AND NOT ONE ===
 *
 * A link has no single "speed". It has a ceiling beyond which it loses, and that
 * ceiling moves. So we publish three things - the highest bitrate seen CLEAN,
 * the lowest seen WITH LOSS, and how many samples back each. A "your bitrate is
 * 17 Mb/s" invents a precision the measurement does not have.
 */
#ifndef SHADOW_PATH_PROBE_H
#define SHADOW_PATH_PROBE_H

#include <stdint.h>
#include <string.h>

/* Above this rate a sample counts as "lossy". Chosen above the
 * background noise: the server routinely resends the last chunk of every picture,
 * and a healthy session measures samples at 0.0 % with spikes of 1-3 % when it
 * saturates. 0.3 % separates the two unambiguously. */
#define PROBE_LOSS_THRESHOLD 0.003f

/* Below this we conclude nothing. A single loss spike can come from an
 * event unrelated to bitrate; it takes several, at different bitrates, before one
 * can speak of a ceiling. */
#define PROBE_MIN_SAMPLES     12
#define PROBE_MIN_LOSSY        3

typedef struct {
    uint32_t samples;        /* echantillons pris */
    uint32_t lossy;          /* of which lossy */
    float    clean_max_mbps; /* highest bitrate seen WITHOUT loss */
    float    lossy_min_mbps; /* lowest bitrate seen WITH loss */
    float    clean_sum, lossy_sum;   /* for the means */
} path_probe_t;

typedef enum {
    PROBE_INSUFFICIENT = 0,  /* not enough samples to say anything */
    PROBE_NO_CEILING,        /* no loss: the path carries at least clean_max */
    PROBE_CEILING_FOUND,     /* a knee is visible */
} probe_verdict_t;

static inline void path_probe_reset(path_probe_t *p)
{
    if (p) memset(p, 0, sizeof *p);
}

/* One sample. `mbps` is the bitrate measured over the interval, `loss` the loss
 * rate of that same interval (0 to 1).
 *
 * Zero-bitrate samples are IGNORED: a paused session, or a stream that never
 * started, would otherwise produce a "ceiling" of zero - which is the opposite
 * of a measurement. */
static inline void path_probe_add(path_probe_t *p, float mbps, float loss)
{
    if (!p || mbps <= 0.1f) return;
    p->samples++;
    if (loss > PROBE_LOSS_THRESHOLD) {
        p->lossy++;
        p->lossy_sum += mbps;
        if (p->lossy_min_mbps == 0.0f || mbps < p->lossy_min_mbps)
            p->lossy_min_mbps = mbps;
    } else {
        p->clean_sum += mbps;
        if (mbps > p->clean_max_mbps) p->clean_max_mbps = mbps;
    }
}

static inline probe_verdict_t path_probe_verdict(const path_probe_t *p)
{
    if (!p || p->samples < PROBE_MIN_SAMPLES) return PROBE_INSUFFICIENT;
    if (p->lossy < PROBE_MIN_LOSSY)           return PROBE_NO_CEILING;
    return PROBE_CEILING_FOUND;
}

/* The ceiling to advise, in Mb/s, or 0 when we do not know.
 *
 * It is the highest bitrate seen CLEAN, and not a mean: advising the mean would
 * advise a bitrate at which half the samples were already losing. Nor the lowest
 * seen lossy - that would advise the exact point where it breaks.
 *
 * When no ceiling has been found we advise NOTHING: the link carries at least
 * what was asked of it, and inventing a higher number would be extrapolation
 * dressed up as measurement. */
static inline float path_probe_suggest_mbps(const path_probe_t *p)
{
    if (path_probe_verdict(p) != PROBE_CEILING_FOUND) return 0.0f;
    return p->clean_max_mbps;
}

static inline float path_probe_clean_avg(const path_probe_t *p)
{
    const uint32_t n = p ? (p->samples - p->lossy) : 0;
    return n ? p->clean_sum / (float)n : 0.0f;
}

static inline float path_probe_lossy_avg(const path_probe_t *p)
{
    return (p && p->lossy) ? p->lossy_sum / (float)p->lossy : 0.0f;
}

#endif /* SHADOW_PATH_PROBE_H */
