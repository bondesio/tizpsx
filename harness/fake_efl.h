/* Host-side fake of the EFL / Ecore / appfw / audio-out calls the emulator
 * frontends hand-declare. #include it once into a harness that links the real
 * frontend .c (compiled with -Dmain=<app>_main). Objects are plain structs, so
 * a test can read what would be on screen: text, position, size, visibility.
 * Superset of what TizGB, TizNES, TizNeo, TizGBA and TizPSX call; unused
 * functions cost nothing. No timing is simulated: fk_pump() calls the app's
 * animators and timers at 60 Hz in real time, so worker threads also run.
 */
#ifndef FRAMEWORK_FAKE_EFL_H
#define FRAMEWORK_FAKE_EFL_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { int used, shown, x, y, w, h, smooth, label, iw, ih; void *buf; char text[1024]; } fobj_t;
static fobj_t g_objs[512];
static int g_nobj;
static void *fk_new(int label) { fobj_t *o = &g_objs[g_nobj++]; o->used = 1; o->label = label; return o; }
#define FO(p) ((fobj_t *)(p))

typedef unsigned char Eina_Bool;
typedef struct { void *create, *terminate, *pause, *resume, *app_control; } fk_callbacks_t;
typedef Eina_Bool (*fk_cb)(void *);
typedef Eina_Bool (*fk_key_cb)(void *, int, void *);

void *elm_win_util_standard_add(const char *a, const char *b) { (void)a; (void)b; return fk_new(0); }
void elm_win_fullscreen_set(void *o, int v) { (void)o; (void)v; }
void elm_win_resize_object_add(void *a, void *b) { (void)a; (void)b; }
void elm_object_focus_set(void *o, Eina_Bool v) { (void)o; (void)v; }
void *evas_object_evas_get(void *o) { return o; }
void evas_output_size_get(const void *e, int *w, int *h) { (void)e; *w = 1920; *h = 1080; }
void *evas_object_rectangle_add(void *e) { (void)e; return fk_new(0); }
void *evas_object_text_add(void *e) { (void)e; return fk_new(0); }
void *evas_object_image_filled_add(void *e) { (void)e; return fk_new(0); }
void *elm_label_add(void *p) { (void)p; return fk_new(1); }
void elm_object_part_text_set(void *o, const char *part, const char *t) { (void)part; snprintf(FO(o)->text, sizeof FO(o)->text, "%s", t ? t : ""); }
void evas_object_image_size_set(void *o, int w, int h)
{
    FO(o)->iw = w; FO(o)->ih = h;
    free(FO(o)->buf); FO(o)->buf = calloc((size_t)(w > 0 ? w : 1) * (size_t)(h > 0 ? h : 1), 4);
}
void *evas_object_image_data_get(const void *o, Eina_Bool for_write) { (void)for_write; return ((const fobj_t *)o)->buf; }
int evas_object_image_stride_get(const void *o) { return ((const fobj_t *)o)->iw * 4; }
void evas_object_image_data_copy_set(void *o, void *d) { if (FO(o)->buf) memcpy(FO(o)->buf, d, (size_t)FO(o)->iw * (size_t)FO(o)->ih * 4); }
void evas_object_image_data_set(void *o, void *d) { (void)o; (void)d; }
static unsigned long fk_frames_presented;
void evas_object_image_data_update_add(void *o, int x, int y, int w, int h) { (void)o; (void)x; (void)y; (void)w; (void)h; fk_frames_presented++; }
void evas_object_image_smooth_scale_set(void *o, Eina_Bool v) { FO(o)->smooth = v; }
void evas_object_image_alpha_set(void *o, Eina_Bool v) { (void)o; (void)v; }
void evas_object_text_font_set(void *o, const char *f, int s) { (void)o; (void)f; (void)s; }
void evas_object_text_text_set(void *o, const char *t) { snprintf(FO(o)->text, sizeof FO(o)->text, "%s", t ? t : ""); }
int evas_object_text_horiz_advance_get(const void *o) { return (int)strlen(((const fobj_t *)o)->text) * 14; }
void evas_object_color_set(void *o, int r, int g, int b, int a) { (void)o; (void)r; (void)g; (void)b; (void)a; }
void evas_object_move(void *o, int x, int y) { FO(o)->x = x; FO(o)->y = y; }
void evas_object_resize(void *o, int w, int h) { FO(o)->w = w; FO(o)->h = h; }
void evas_object_show(void *o) { FO(o)->shown = 1; }
void evas_object_hide(void *o) { FO(o)->shown = 0; }

int ECORE_EVENT_KEY_DOWN = 10, ECORE_EVENT_KEY_UP = 11;
static fk_key_cb fk_key_down, fk_key_up;
void *ecore_event_handler_add(int type, fk_key_cb cb, const void *d) { (void)d; if (type == 10) fk_key_down = cb; else fk_key_up = cb; return (void *)1; }
void ecore_event_handler_del(void *h) { (void)h; }
typedef struct { fk_cb cb; const void *data; double interval, acc; int live; } fk_tm_t;
static fk_tm_t fk_tm[8];
static int fk_ntm;
void *ecore_animator_add(fk_cb cb, const void *d) { fk_tm[fk_ntm] = (fk_tm_t){ cb, d, 0.0, 0.0, 1 }; return &fk_tm[fk_ntm++]; }
void *ecore_timer_add(double t, fk_cb cb, const void *d) { fk_tm[fk_ntm] = (fk_tm_t){ cb, d, t, 0.0, 1 }; return &fk_tm[fk_ntm++]; }
void ecore_animator_del(void *a) { ((fk_tm_t *)a)->live = 0; }
void ecore_timer_del(void *a) { ((fk_tm_t *)a)->live = 0; }

static int fk_ui_exit;
void ui_app_exit(void) { fk_ui_exit = 1; }
static fk_callbacks_t fk_cb_set;
static void fk_run_scenarios(void);
int ui_app_main(int argc, char **argv, const fk_callbacks_t *cb, void *d)
{
    (void)argc; (void)argv; (void)d;
    fk_cb_set = *cb;
    fk_run_scenarios();
    return 0;
}

typedef struct { const char *keyname, *key, *string; } fk_key_ev;
static void fk_key(const char *name)
{
    fk_key_ev e = { name, name, name };
    if (fk_key_down) fk_key_down(0, 10, &e);
    if (fk_key_up) fk_key_up(0, 11, &e);
}
/* Advance `seconds` of real time at 60 Hz: animators every tick, timers on their interval. */
static void fk_pump(double seconds)
{
    int ticks = (int)(seconds * 60.0 + 0.5);
    for (int i = 0; i < ticks; i++) {
        for (int t = 0; t < fk_ntm; t++) {
            fk_tm_t *m = &fk_tm[t];
            if (!m->live) continue;
            m->acc += 1.0 / 60.0;
            if (m->interval <= 0.0 || m->acc >= m->interval) {
                m->acc = 0.0;
                if (!m->cb((void *)m->data)) m->live = 0;
            }
        }
        struct timespec d = { 0, 1000000000L / 60 };
        nanosleep(&d, 0);
    }
}

/* audio-out: accepts everything, discards samples, never calls back. */
int audio_out_create_new(int rate, int ch, int type, void **h) { (void)rate; (void)ch; (void)type; *h = (void *)1; return 0; }
int audio_out_set_stream_cb(void *h, void *cb, void *d) { (void)h; (void)cb; (void)d; return 0; }
int audio_out_prepare(void *h) { (void)h; return 0; }
int audio_out_unprepare(void *h) { (void)h; return 0; }
int audio_out_destroy(void *h) { (void)h; return 0; }
int audio_out_write(void *h, void *buf, unsigned n) { (void)h; (void)buf; return (int)n; }

/* Test helpers. */
static int fk_pass, fk_fail;
#define FK_CHECK(cond, ...) do { if (cond) { fk_pass++; printf("PASS "); } else { fk_fail++; printf("FAIL "); } printf(__VA_ARGS__); printf("\n"); } while (0)
static fobj_t *fk_find_shown(const char *needle)
{
    for (int i = 0; i < g_nobj; i++) if (g_objs[i].shown && strstr(g_objs[i].text, needle)) return &g_objs[i];
    return 0;
}
static fobj_t *fk_cursor(void)   /* the 8-px-wide library cursor bar */
{
    fobj_t *c = 0;
    for (int i = 0; i < g_nobj; i++) if (g_objs[i].shown && g_objs[i].w == 8) c = &g_objs[i];
    return c;
}
#endif
