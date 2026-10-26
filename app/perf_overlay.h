/* Copy into an emulator's frontend. Header-only plain C with no #includes, so
 * it builds under the apps' -nostdinc recipes: the includer must already
 * declare snprintf(char *, unsigned long, const char *, ...). The caller
 * supplies monotonic nanoseconds and draws the text.
 * See docs/overlay.md for where to call it and how to draw the result.
 */
#ifndef EMULATOR_STARTER_PERF_OVERLAY_H
#define EMULATOR_STARTER_PERF_OVERLAY_H

/* Persisted Settings value (key "perf_overlay"); Off must stay 0. */
typedef enum {
    PERF_OVERLAY_OFF, PERF_OVERLAY_FPS, PERF_OVERLAY_DETAILED, PERF_OVERLAY_MODES
} perf_overlay_mode;

#define PERF_WINDOW_NS 1000000000ULL

typedef struct {
    unsigned long long win_start_ns;
    unsigned int frames, late;
    unsigned long long work_sum_ns, work_max_ns;
    /* Published once per window; the only fields the overlay reads. */
    int      valid;
    unsigned int fps_x100, work_avg_us, work_max_us, late_frames;
} perf_stats;

static inline perf_overlay_mode perf_mode_sanitize(int v)
{
    return (v >= 0 && v < PERF_OVERLAY_MODES) ? (perf_overlay_mode)v : PERF_OVERLAY_OFF;
}

static inline perf_overlay_mode perf_mode_next(perf_overlay_mode m)
{
    return (perf_overlay_mode)((m + 1) % PERF_OVERLAY_MODES);
}

static inline const char *perf_mode_label(perf_overlay_mode m)
{
    static const char *const label[PERF_OVERLAY_MODES] = { "Off", "FPS", "Detailed" };
    return label[perf_mode_sanitize((int)m)];
}

/* Call when a game starts, resumes from a menu, or the core is reset. */
static inline void perf_reset(perf_stats *p, unsigned long long now_ns)
{
    *p = (perf_stats){ .win_start_ns = now_ns };
}

/* Call once per emulated frame. work_ns is the time spent stepping the core and
 * converting/presenting that frame; late is non-zero if the frame missed its
 * deadline. Returns 1 when a new one-second window was published, so the caller
 * updates the on-screen text only then, never per frame.
 */
static inline int perf_frame(perf_stats *p, unsigned long long now_ns, unsigned long long work_ns, int late)
{
    p->frames++;
    p->late += late ? 1u : 0u;
    p->work_sum_ns += work_ns;
    if (work_ns > p->work_max_ns) p->work_max_ns = work_ns;

    unsigned long long span = now_ns - p->win_start_ns;
    if (now_ns < p->win_start_ns || span < PERF_WINDOW_NS) return 0;

    p->fps_x100    = (unsigned int)((unsigned long long)p->frames * 100000000000ULL / span);
    p->work_avg_us = (unsigned int)(p->work_sum_ns / p->frames / 1000u);
    p->work_max_us = (unsigned int)(p->work_max_ns / 1000u);
    p->late_frames = p->late;
    p->valid = 1;

    p->win_start_ns = now_ns;
    p->frames = p->late = 0;
    p->work_sum_ns = p->work_max_ns = 0;
    return 1;
}

/* Formats the overlay text; returns its length, 0 when nothing should be drawn
 * (mode Off, or no window published yet). target_fps_x100 is the core's native
 * rate (e.g. 5994); audio_underruns is the app's running count, or 0 if unknown.
 */
static inline int perf_format(const perf_stats *p, perf_overlay_mode mode,
                              unsigned int target_fps_x100, unsigned int audio_underruns,
                              char *out, unsigned long cap)
{
    if (mode == PERF_OVERLAY_OFF || !p->valid || cap == 0) {
        if (cap) out[0] = '\0';
        return 0;
    }
    if (mode == PERF_OVERLAY_FPS)
    {
        unsigned int tenths = (p->fps_x100 + 5) / 10;  /* round, don't truncate 59.99 to 59.9 */
        return snprintf(out, cap, "%u.%u FPS", tenths / 10, tenths % 10);
    }
    return snprintf(out, cap,
        "%u.%02u / %u.%02u FPS  work %u.%u ms (max %u.%u)  late %u  audio under %u",
        p->fps_x100 / 100, p->fps_x100 % 100, target_fps_x100 / 100, target_fps_x100 % 100,
        p->work_avg_us / 1000, (p->work_avg_us % 1000) / 100,
        p->work_max_us / 1000, (p->work_max_us % 1000) / 100,
        p->late_frames, audio_underruns);
}

#endif
