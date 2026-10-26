/* Host harness for ui/psx_main.c: the real frontend compiled unchanged
 * (-Dmain=tizpsx_app_main) against harness/fake_efl.h, a fake
 * input/library/core. Checks the library Exit row, the Performance overlay
 * setting, its persistence and its on-screen text while the real worker
 * thread runs the real RGB565 conversion + edge smoothing. */
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include "../app/psx_core.h"
#include "../app/input.h"
#include "../app/library.h"
#include "fake_efl.h"

#define DATA "out/data"
static const char *g_names[3] = { "Alpha Racing", "Beta Quest", "Gamma Fighters" };

/* ---- fake library ---- */
static psx_library_item g_items[3];
void psx_library_scan(void)
{
    for (int i = 0; i < 3; i++) { snprintf(g_items[i].name, sizeof g_items[i].name, "%s", g_names[i]); snprintf(g_items[i].path, sizeof g_items[i].path, "/games/%d.cue", i); }
}
int psx_library_count(void) { return 3; }
const psx_library_item *psx_library_get(int i) { return i >= 0 && i < 3 ? &g_items[i] : 0; }
int psx_library_is_bios_candidate(const psx_library_item *it) { (void)it; return 0; }

/* ---- fake input ---- */
unsigned psx_input_mapping_binding(unsigned s) { return s; }
void psx_input_init(const char *p) { (void)p; }
void psx_input_shutdown(void) {}
void psx_input_pause_capture(void) {}
void psx_input_resume_capture(void) {}
void psx_input_poll(unsigned long long n) { (void)n; }
int psx_input_recent_activity(unsigned long long n) { (void)n; return 0; }
uint16_t psx_input_buttons(unsigned p) { (void)p; return 0; }
int16_t psx_input_analog(unsigned p, unsigned a) { (void)p; (void)a; return 0; }
int psx_input_rumble_enabled(void) { return 0; }
void psx_input_set_rumble_enabled(int e) { (void)e; }
void psx_input_rumble_stop(void) {}
const char *psx_input_rumble_status(unsigned p) { (void)p; return "rumble n/a"; }
const char *psx_input_status(unsigned p) { (void)p; return "none"; }
const char *psx_input_error(void) { return ""; }
int psx_input_changed(void) { return 0; }
void psx_input_cycle_device(unsigned p, int d) { (void)p; (void)d; }
void psx_input_device_label(unsigned p, char *o, size_t c) { (void)p; snprintf(o, c, "none"); }
void psx_input_binding_label(unsigned p, unsigned b, char *o, size_t c) { (void)p; (void)b; snprintf(o, c, "-"); }
const char *psx_input_binding_name(unsigned b) { (void)b; return "x"; }
void psx_input_capture(unsigned p, unsigned b, unsigned long long n) { (void)p; (void)b; (void)n; }
void psx_input_cancel_capture(void) {}
int psx_input_capturing(void) { return 0; }
int psx_input_capture_succeeded(void) { return 0; }
int psx_input_exclusive(void) { return 1; }
void psx_input_toggle_exclusive(void) {}
const char *psx_input_capture_status(unsigned p) { (void)p; return "capture n/a"; }
void psx_input_test_begin(unsigned p) { (void)p; }
void psx_input_test_cancel(void) {}
void psx_input_test_skip(void) {}
void psx_input_test_label(char *o, size_t c) { snprintf(o, c, "-"); }

/* ---- fake core: 640x480 RGB565 with hard edges so edge smoothing has work ---- */
static unsigned g_core_frames;
int psx_core_bios_supported(const char *p) { (void)p; return 1; }
int psx_core_init(const char *b, const char *c, unsigned m) { (void)b; (void)c; (void)m; g_core_frames = 0; return 0; }
int psx_core_run(uint32_t a, uint32_t b, const int16_t *x, const int16_t *y, psx_video_frame_t *v, psx_audio_frame_t *au)
{
    (void)a; (void)b; (void)x; (void)y;
    v->width = 640; v->height = 480; v->pitch_pixels = 640; v->valid = 1;
    for (unsigned r = 0; r < 480; r++) for (unsigned c = 0; c < 640; c++)
        v->pixels[r * 640 + c] = (((c / 16 + r / 16 + g_core_frames) & 1) ? 0xf81f : 0x07e0) ^ (uint16_t)(c * 3);
    au->frames = 735;
    for (unsigned i = 0; i < 735 * 2; i++) au->samples[i] = 0;
    g_core_frames++;
    return 0;
}
void psx_core_set_analog_mode(int e) { (void)e; }
unsigned psx_core_frame_rate_millihz(void) { return 59940; }
void psx_core_deinit(void) {}
const char *psx_core_error(void) { return "fake core"; }

extern int tizpsx_app_main(int, char **);
static int file_has(const char *path, const char *needle)
{
    char buf[2048] = ""; FILE *f = fopen(path, "r"); size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0; if (f) fclose(f); buf[n] = 0;
    return strstr(buf, needle) != 0;
}
static fobj_t *row(const char *text) { return fk_find_shown(text); }

static void fk_run_scenarios(void)
{
    ((void (*)(void *))fk_cb_set.create)(0);
    printf("-- library\n");
    fobj_t *exit_row = row("Exit TizPSX");
    FK_CHECK(exit_row && exit_row->y == 230, "Exit TizPSX is the first library row (y=%d)", exit_row ? exit_row->y : -1);
    FK_CHECK(row("Alpha Racing") && row("Alpha Racing")->y == 230 + 52 && row("Settings and controller setup")->y == 230 + 4 * 52,
             "Exit, 3 games, Settings in order");
    FK_CHECK(fk_cursor() && fk_cursor()->y == 236 + 52, "startup selection is the first game, not Exit (cursor y=%d)", fk_cursor() ? fk_cursor()->y : -1);
    fk_key("Up");
    FK_CHECK(fk_cursor()->y == 236 && fk_find_shown("closes TizPSX"), "Up reaches Exit; footer says OK closes TizPSX");
    fk_key("Down"); fk_key("Down"); fk_key("Down"); fk_key("Down"); fk_key("Down");
    FK_CHECK(fk_cursor()->y == 236 + 4 * 52, "Down stops on the Settings row (last)");

    printf("-- settings\n");
    fk_key("Return");
    FK_CHECK(fk_find_shown("TizPSX settings") != 0, "Settings opens from the last row");
    for (int i = 0; i < 15; i++) fk_key("Down");
    FK_CHECK(fk_find_shown("Performance overlay: Off") && fk_find_shown("Off, FPS, or Detailed"), "overlay row in SYSTEM, default Off");
    fk_key("Right");
    FK_CHECK(fk_find_shown("Performance overlay: FPS") && file_has(DATA "/settings.ini", "perf_overlay=1"), "overlay -> FPS, persisted in settings.ini");
    fk_key("Right"); fk_key("Right");
    FK_CHECK(fk_find_shown("Performance overlay: Off"), "overlay cycles FPS > Detailed > Off");
    fk_key("Left");
    FK_CHECK(fk_find_shown("Performance overlay: Detailed") && file_has(DATA "/settings.ini", "perf_overlay=2"), "overlay Left from Off -> Detailed, persisted");
    FK_CHECK(file_has(DATA "/settings.ini", "antialiasing=1") && file_has(DATA "/settings.ini", "resolution_multiplier=2"), "existing settings preserved alongside");
    fk_key("Escape");

    printf("-- game with Detailed overlay (real worker, 640x480, edge smoothing on)\n");
    FK_CHECK(fk_cursor()->y == 236 + 4 * 52, "back in the library on the Settings row");
    for (int i = 0; i < 3; i++) fk_key("Up");             /* Alpha Racing */
    fk_key("Return");
    fobj_t *perf = fk_find_shown(" late ");
    FK_CHECK(!perf, "overlay hidden until the first 1 s window closes");
    fk_pump(2.6);
    perf = fk_find_shown(" late ");
    FK_CHECK(perf && strstr(perf->text, " / 59.94 FPS"), "Detailed overlay while playing: %s", perf ? perf->text : "(none)");
    FK_CHECK(perf && perf->y == 44 && perf->x + (int)strlen(perf->text) * 14 == 1920 - 48, "right-aligned 48 px from the edge, y=44");
    unsigned fps_x100 = 0; if (perf) { unsigned a = 0, b = 0; if (sscanf(perf->text, "%u.%u", &a, &b) == 2) fps_x100 = a * 100 + b; }
    FK_CHECK(fps_x100 >= 5700 && fps_x100 <= 6100, "emulated rate near 59.94 even with edge smoothing at 2x (%u.%02u)", fps_x100 / 100, fps_x100 % 100);
    fk_key("Escape");
    FK_CHECK(!fk_find_shown(" late ") && fk_find_shown("Exit TizPSX"), "Back stops the game: overlay hidden, library shown");

    printf("-- FPS mode and Off\n");
    fk_key("Down"); fk_key("Down"); fk_key("Down"); fk_key("Return");   /* Settings */
    for (int i = 0; i < 15; i++) fk_key("Down");
    fk_key("Left");                                        /* Detailed -> FPS */
    fk_key("Escape");
    for (int i = 0; i < 3; i++) fk_key("Up");
    fk_key("Return");
    fk_pump(2.2);
    perf = fk_find_shown(" FPS");
    FK_CHECK(perf && !strstr(perf->text, "late") && !strstr(perf->text, " / "), "FPS mode shows only the rate: %s", perf ? perf->text : "(none)");
    fk_key("Escape");
    fk_key("Down"); fk_key("Down"); fk_key("Down"); fk_key("Return");
    for (int i = 0; i < 15; i++) fk_key("Down");
    fk_key("Left");                                        /* FPS -> Off */
    fk_key("Escape");
    for (int i = 0; i < 3; i++) fk_key("Up");
    fk_key("Return");
    fk_pump(2.2);
    FK_CHECK(!fk_find_shown(" FPS"), "overlay Off: nothing drawn in game");
    fk_key("Escape");

    printf("-- Exit row\n");
    for (int i = 0; i < 6; i++) fk_key("Up");
    fk_ui_exit = 0;
    fk_key("Return");
    FK_CHECK(fk_ui_exit, "OK on the Exit row closes the app");
    printf("\npsx ui_test: %d passed, %d failed\n", fk_pass, fk_fail);
}

int main(void)
{
    system("rm -rf out && mkdir -p out/data");
    FILE *f = fopen(DATA "/settings.ini", "w");
    fputs("volume=80\nmute=0\ninteger=1\nfilter=0\nresolution_multiplier=2\nantialiasing=1\ndualshock_analog=1\nbios=/fake/bios.bin\n", f);
    fclose(f);
    tizpsx_app_main(0, 0);
    return fk_fail ? 1 : 0;
}
