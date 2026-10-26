/* TizPSX native Evas frontend. The core runs on a paced worker; Evas stays on
 * the UI thread and consumes the newest complete frame. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include "../psx_core.h"
#include "../library.h"
#include "../input.h"

typedef unsigned char Eina_Bool;
typedef struct { void *create, *terminate, *pause, *resume, *app_control; } callbacks_t;
typedef void Evas_Object;
typedef void Evas;
typedef struct { const char *keyname, *key, *string; } key_event_t;
typedef void *audio_out_h;

extern int ui_app_main(int, char **, const callbacks_t *, void *);
extern Evas_Object *elm_win_util_standard_add(const char *, const char *);
extern Evas_Object *evas_object_rectangle_add(Evas *);
extern Evas_Object *evas_object_text_add(Evas *);
extern Evas_Object *evas_object_image_filled_add(Evas *);
extern Evas *evas_object_evas_get(Evas_Object *);
extern void elm_win_fullscreen_set(Evas_Object *, int);
extern void elm_win_resize_object_add(Evas_Object *, Evas_Object *);
extern void elm_object_focus_set(Evas_Object *, Eina_Bool);
extern void evas_object_color_set(Evas_Object *, int, int, int, int);
extern void evas_object_move(Evas_Object *, int, int);
extern void evas_object_resize(Evas_Object *, int, int);
extern void evas_object_show(Evas_Object *);
extern void evas_object_hide(Evas_Object *);
extern void evas_object_text_font_set(Evas_Object *, const char *, int);
extern void evas_object_text_text_set(Evas_Object *, const char *);
extern void evas_object_image_alpha_set(Evas_Object *, Eina_Bool);
extern void evas_object_image_size_set(Evas_Object *, int, int);
extern void evas_object_image_data_set(Evas_Object *, void *);
extern void evas_object_image_data_update_add(Evas_Object *, int, int, int, int);
extern void evas_object_image_smooth_scale_set(Evas_Object *, Eina_Bool);
extern void *ecore_event_handler_add(int, Eina_Bool (*)(void *, int, void *), const void *);
extern void *ecore_timer_add(double, Eina_Bool (*)(void *), const void *);
extern int ECORE_EVENT_KEY_DOWN;
extern void ui_app_exit(void);
extern int audio_out_create_new(int, int, int, audio_out_h *);
extern int audio_out_set_stream_cb(audio_out_h, void (*)(audio_out_h, unsigned, void *), void *);
extern int audio_out_prepare(audio_out_h);
extern int audio_out_unprepare(audio_out_h);
extern int audio_out_destroy(audio_out_h);
extern int audio_out_write(audio_out_h, void *, unsigned);
extern int open(const char *, int, ...);
extern int close(int);
extern long read(int, void *, unsigned long);
extern long write(int, const void *, unsigned long);
extern long lseek(int, long, int);
extern int snprintf(char *, unsigned long, const char *, ...);
#include "../perf_overlay.h"
#include "video_convert.h"
extern int evas_object_text_horiz_advance_get(const Evas_Object *);
extern int clock_gettime(int, struct timespec *);
extern int nanosleep(const struct timespec *, struct timespec *);
extern void *malloc(unsigned long);
extern void free(void *);

#ifndef TIZPSX_DATA_DIR
#define TIZPSX_DATA_DIR "/opt/usr/home/owner/apps_rw/org.pablo.tizpsx/data"
#endif
#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT 0x40
#define O_TRUNC 0x200
#define ROWS 20
#define LIBRARY_VISIBLE 14
#define PATH_CAP 512
#define AUDIO_RING_FRAMES 8192u
#define AUDIO_CHANNEL_STEREO 0x81
#define AUDIO_SAMPLE_TYPE_S16LE 0x71
#define CLOCK_MONOTONIC 1
#define WORKER_MAX_CATCHUP 3u
#define WORKER_MAX_LAG 6u
#define PIXEL_CAP (PSX_CORE_MAX_WIDTH * PSX_CORE_MAX_HEIGHT)

enum { LIBRARY, SETTINGS, PICKER, GAME, MESSAGE, MAPPING, INPUT_TEST };
enum { P1_DEVICE, P2_DEVICE, EXCLUSIVE, MAP_P1, MAP_P2, INPUT_DIAG,
       RUMBLE, ANALOG_MODE, VOLUME, MUTE, SCREEN, RESOLUTION, EDGE_SMOOTHING, FILTER,
       BIOS, PERF_OVERLAY, RESCAN, BACK, EXIT_APP, SET_COUNT };
typedef struct { const char *title; const int *items; int count; } settings_section;
static const int controller_rows[] = { P1_DEVICE, P2_DEVICE, EXCLUSIVE, MAP_P1, MAP_P2, INPUT_DIAG, RUMBLE, ANALOG_MODE };
static const int audio_rows[] = { VOLUME, MUTE };
static const int video_rows[] = { SCREEN, RESOLUTION, EDGE_SMOOTHING, FILTER };
static const int system_rows[] = { BIOS, PERF_OVERLAY, RESCAN, BACK, EXIT_APP };
static const settings_section sections[] = {
    { "CONTROLLERS", controller_rows, 8 }, { "AUDIO", audio_rows, 2 },
    { "VIDEO", video_rows, 4 }, { "SYSTEM", system_rows, 5 }
};
#define SECTION_COUNT ((int)(sizeof sections / sizeof sections[0]))

static Evas_Object *bg, *heading, *footer, *cursor, *image, *rows[ROWS];
static Evas_Object *set_heads[SECTION_COUNT], *set_rules[SECTION_COUNT];
static Evas *evas;
static int sw = 1920, sh = 1080;
static int page = LIBRARY, libsel, setsel, picksel, volume = 80;
static int mute, integer = 1, filter, resolution_multiplier = 1, antialiasing, playing, analog_mode = 1;
static int mapping_player, mapping_step, mapping_active, input_test_player;
static char bios[PATH_CAP], autostart[PATH_CAP], notice[256];
static audio_out_h audio_handle;
static int perf_mode;                       /* perf_overlay_mode: Off / FPS / Detailed */
static perf_stats perf_pub;                 /* worker -> UI snapshot, guarded by frame_mutex */
static unsigned perf_pub_seq, perf_shown_seq, perf_native_x100 = 6000;
static Evas_Object *perf_bg, *perf_txt;

/* Allocate large frame buffers only after PCSX initializes its dynarec. Its
 * ARM branch encodings need the executable's end address within 32 MiB; large
 * static video arrays moved that address and made game boot abort. */
static uint32_t (*argb_frames)[PIXEL_CAP];
static uint32_t *argb_display, *aa_scratch;
static uint8_t *aa_luma;
static unsigned frame_width, frame_height, published_index, frame_sequence;
static unsigned last_presented_sequence;
static pthread_mutex_t frame_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Core objects are touched only by the emulation worker after init, and only
 * after that worker is joined during deinit, pause and shutdown. */
static psx_video_frame_t *video;
static psx_audio_frame_t audio;
static pthread_t worker_thread;
static unsigned worker_started;
static volatile unsigned worker_stop, worker_failed, worker_buttons[2];
static int16_t worker_analog[2][4];
static char worker_error[256];
static volatile unsigned buttons;
static int16_t audio_ring[AUDIO_RING_FRAMES * 2u];
static unsigned audio_read, audio_write;

/* Ten-second low-rate diagnostics separate core time, pixel conversion,
 * Ecore callbacks and Evas submissions without per-frame file writes. */
static unsigned long long perf_start_ns, perf_frames, perf_core_ns;
static unsigned long long perf_convert_ns, perf_core_max_ns, perf_late;
static unsigned perf_ui_ticks, perf_presented;

__attribute__((weak)) int psx_core_init(const char *, const char *, unsigned);
__attribute__((weak)) int psx_core_run(uint32_t, uint32_t, const int16_t *, const int16_t *, psx_video_frame_t *, psx_audio_frame_t *);
__attribute__((weak)) unsigned psx_core_frame_rate_millihz(void);
__attribute__((weak)) void psx_core_set_analog_mode(int);
__attribute__((weak)) void psx_core_deinit(void);
__attribute__((weak)) const char *psx_core_error(void);

static unsigned long long now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 0;
    return (unsigned long long)ts.tv_sec * 1000000000ULL + (unsigned long long)ts.tv_nsec;
}

void psx_diag(const char *line)
{
    char path[PATH_CAP];
    snprintf(path, sizeof path, "%s/tizpsx-runtime.log", TIZPSX_DATA_DIR);
    int fd = open(path, O_WRONLY | O_CREAT, 0600);
    if (fd < 0) return;
    lseek(fd, 0, 2);
    write(fd, line, strlen(line));
    write(fd, "\n", 1);
    close(fd);
}
#define diag psx_diag


static void frame_storage_close(void)
{
    if (image) {
        evas_object_hide(image);
        evas_object_image_data_set(image, NULL);
    }
    free(video); video = NULL;
    free(argb_frames); argb_frames = NULL;
    free(argb_display); argb_display = NULL;
    free(aa_scratch); aa_scratch = NULL;
    free(aa_luma); aa_luma = NULL;
}

static int frame_storage_open(void)
{
    size_t bytes = (size_t)PIXEL_CAP * sizeof(uint32_t);
    video = malloc(sizeof(*video));
    argb_frames = malloc(bytes * 2u);
    argb_display = malloc(bytes);
    aa_scratch = malloc(bytes);
    aa_luma = malloc(PIXEL_CAP);
    if (!video || !argb_frames || !argb_display || !aa_scratch || !aa_luma) {
        frame_storage_close();
        return 0;
    }
    memset(video, 0, sizeof(*video));
    return 1;
}

static void save_settings(void)
{
    char data[700];
    int len = snprintf(data, sizeof data,
        "volume=%d\nmute=%d\ninteger=%d\nfilter=%d\nresolution_multiplier=%d\nantialiasing=%d\ndualshock_analog=%d\nperf_overlay=%d\nbios=%s\n",
        volume, mute, integer, filter, resolution_multiplier, antialiasing, analog_mode, perf_mode, bios);
    int fd = open(TIZPSX_DATA_DIR "/settings.ini", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return;
    write(fd, data, (unsigned long)len);
    close(fd);
}

static int number(const char *s)
{
    int n = 0;
    while (*s >= '0' && *s <= '9') n = n * 10 + *s++ - '0';
    return n;
}

static void load_settings(void)
{
    char data[1024];
    int fd = open(TIZPSX_DATA_DIR "/settings.ini", O_RDONLY);
    if (fd < 0) return;
    long len = read(fd, data, sizeof data - 1);
    close(fd);
    if (len <= 0) return;
    data[len] = 0;
    char *line = data;
    while (*line) {
        char *end = line;
        while (*end && *end != '\n') ++end;
        if (*end) *end++ = 0;
        if (!__builtin_strncmp(line, "volume=", 7)) volume = number(line + 7);
        else if (!__builtin_strncmp(line, "mute=", 5)) mute = number(line + 5);
        else if (!__builtin_strncmp(line, "integer=", 8)) integer = number(line + 8);
        else if (!__builtin_strncmp(line, "filter=", 7)) filter = number(line + 7);
        else if (!__builtin_strncmp(line, "resolution_multiplier=", 22)) resolution_multiplier = number(line + 22);
        else if (!__builtin_strncmp(line, "antialiasing=", 13)) antialiasing = number(line + 13);
        else if (!__builtin_strncmp(line, "dualshock_analog=", 17)) analog_mode = number(line + 17);
        else if (!__builtin_strncmp(line, "perf_overlay=", 13)) perf_mode = number(line + 13);
        else if (!__builtin_strncmp(line, "bios=", 5)) snprintf(bios, sizeof bios, "%s", line + 5);
        else if (!__builtin_strncmp(line, "autostart=", 10)) snprintf(autostart, sizeof autostart, "%s", line + 10);
        line = end;
    }
    if (volume < 0 || volume > 100) volume = 80;
    integer = !!integer;
    filter = !!filter;
    if (resolution_multiplier < 1 || resolution_multiplier > 4) resolution_multiplier = 1;
    antialiasing = !!antialiasing;
    mute = !!mute;
    analog_mode = !!analog_mode;
    perf_mode = perf_mode_sanitize(perf_mode);
}

static void relocate_bios_after_remount(void)
{
    if (psx_core_bios_supported(bios)) return;
    for (int i = 0; i < psx_library_count(); ++i) {
        const psx_library_item *item = psx_library_get(i);
        if (!item || !psx_library_is_bios_candidate(item) ||
            !psx_core_bios_supported(item->path)) continue;
        snprintf(bios, sizeof bios, "%s", item->path);
        save_settings();
        diag("settings: relocated BIOS to currently mounted storage path");
        return;
    }
    if (bios[0]) diag("settings: saved BIOS path unavailable; no valid BIOS found in scanned storage");
}

static void put(Evas_Object *o, const char *text, int x, int y, int size, int active)
{
    evas_object_text_font_set(o, "Sans", size * sw / 1920);
    evas_object_text_text_set(o, text);
    evas_object_color_set(o, active ? 255 : 185, active ? 255 : 185,
                         active ? 255 : 185, 255);
    evas_object_move(o, x * sw / 1920, y * sh / 1080);
    evas_object_show(o);
}

static const char *setting_name(int id)
{
    switch (id) {
    case P1_DEVICE: return "Player 1 controller";
    case P2_DEVICE: return "Player 2 controller";
    case EXCLUSIVE: return "Exclusive gamepad capture";
    case MAP_P1: return "Map Player 1 DualShock controls";
    case MAP_P2: return "Map Player 2 DualShock controls";
    case INPUT_DIAG: return "Guided gamepad input test";
    case RUMBLE: return "Controller rumble";
    case ANALOG_MODE: return "DualShock controller type";
    case VOLUME: return "Volume";
    case MUTE: return "Mute";
    case SCREEN: return "Screen size";
    case FILTER: return "Filtering";
    case RESOLUTION: return "Internal resolution";
    case EDGE_SMOOTHING: return "Edge smoothing";
    case BIOS: return "BIOS file";
    case PERF_OVERLAY: return "Performance overlay";
    case RESCAN: return "Rescan games";
    case BACK: return "Back to library";
    case EXIT_APP: return "Exit TizPSX";
    default: return "";
    }
}

static void hide_settings(void)
{
    for (int i = 0; i < SET_COUNT; ++i) evas_object_hide(rows[i]);
    for (int i = 0; i < SECTION_COUNT; ++i) {
        evas_object_hide(set_heads[i]);
        evas_object_hide(set_rules[i]);
    }
}

static void draw_settings(void)
{
    char text[256], controller[180];
    evas_object_text_text_set(heading, "TizPSX settings");
    for (int i = 0; i < ROWS; ++i) evas_object_hide(rows[i]);
    for (int i = 0; i < SECTION_COUNT; ++i) {
        evas_object_hide(set_heads[i]);
        evas_object_hide(set_rules[i]);
    }
    int y = 150, selected_y = y;
    for (int s = 0; s < SECTION_COUNT; ++s) {
        if (s) y += 8;
        evas_object_text_text_set(set_heads[s], sections[s].title);
        evas_object_text_font_set(set_heads[s], "Sans", 26 * sw / 1920);
        evas_object_color_set(set_heads[s], 120, 200, 255, 255);
        evas_object_move(set_heads[s], 360, y);
        evas_object_show(set_heads[s]);
        evas_object_color_set(set_rules[s], 120, 200, 255, 255);
        evas_object_move(set_rules[s], 360, y + 30);
        evas_object_resize(set_rules[s], 1200, 2);
        evas_object_show(set_rules[s]);
        y += 38;
        for (int r = 0; r < sections[s].count; ++r) {
            int id = sections[s].items[r];
            const char *value = "";
            switch (id) {
            case P1_DEVICE: case P2_DEVICE:
                psx_input_device_label(id == P1_DEVICE ? 0 : 1, controller, sizeof controller);
                snprintf(text, sizeof text, "%s: %.150s", setting_name(id), controller);
                break;
            case EXCLUSIVE:
                snprintf(text, sizeof text, "Exclusive gamepad capture: %s", psx_input_exclusive() ? "On" : "Off"); break;
            case MAP_P1: case MAP_P2:
                snprintf(text, sizeof text, "Map Player %d controls (D-pad, face, L/R, sticks, Select, Start)", id == MAP_P1 ? 1 : 2); break;
            case INPUT_DIAG:
                snprintf(text, sizeof text, "Guided gamepad input test (all 16 buttons)"); break;
            case RUMBLE:
                snprintf(text, sizeof text, "Controller rumble: %s  |  P1: %.72s", psx_input_rumble_enabled() ? "On" : "Off", psx_input_rumble_status(0)); break;
            case ANALOG_MODE:
                snprintf(text, sizeof text, "DualShock type: %s", analog_mode ? "Analog (sticks enabled)" : "Digital"); break;
            case VOLUME:
                snprintf(text, sizeof text, "Volume: %d%%", volume); break;
            case MUTE:
                snprintf(text, sizeof text, "Mute: %s", mute ? "On" : "Off"); break;
            case SCREEN:
                snprintf(text, sizeof text, "Screen size: %s", integer ? "Integer scale" : "Fit screen"); break;
            case RESOLUTION:
                snprintf(text, sizeof text, "Internal resolution: %dx%s", resolution_multiplier,
                    resolution_multiplier == 1 ? " (native)" : " (enhanced 3D)"); break;
            case EDGE_SMOOTHING:
                snprintf(text, sizeof text, "Edge smoothing: %s", antialiasing ? "On" : "Off"); break;
            case FILTER:
                snprintf(text, sizeof text, "Filtering: %s", filter ? "Bilinear" : "Nearest (sharp pixels)"); break;
            case BIOS:
                value = bios[0] ? strrchr(bios, '/') : 0;
                snprintf(text, sizeof text, "BIOS file: %.170s", value ? value + 1 : bios[0] ? bios : "Select a user supplied .bin"); break;
            case PERF_OVERLAY: snprintf(text, sizeof text, "Performance overlay: %s", perf_mode_label((perf_overlay_mode)perf_mode)); break;
            case RESCAN: snprintf(text, sizeof text, "Rescan games, including mounted USB drives"); break;
            case BACK: snprintf(text, sizeof text, "Back to library"); break;
            case EXIT_APP: snprintf(text, sizeof text, "Exit TizPSX"); break;
            default: text[0] = 0; break;
            }
            evas_object_text_text_set(rows[id], text);
            evas_object_text_font_set(rows[id], "Sans", 30 * sw / 1920);
            evas_object_move(rows[id], 390, y);
            evas_object_color_set(rows[id], id == setsel ? 255 : 185,
                                  id == setsel ? 255 : 185, id == setsel ? 255 : 185, 255);
            evas_object_show(rows[id]);
            if (id == setsel) selected_y = y;
            y += 34;
        }
    }
    evas_object_color_set(cursor, 120, 200, 255, 255);
    evas_object_move(cursor, 364, selected_y + 4);
    evas_object_resize(cursor, 8, 28);
    evas_object_show(cursor);
    const char *hint = "D-pad moves   Left/Right changes   Cross selects   Circle returns";
    if (setsel == P1_DEVICE || setsel == P2_DEVICE)
        hint = psx_input_capture_status(setsel == P1_DEVICE ? 0 : 1);
    else if (setsel == EXCLUSIVE)
        hint = psx_input_exclusive() ? "Exclusive capture blocks Tizen gamepad shortcuts when available" : "Shared capture may allow Tizen gamepad shortcuts";
    else if (setsel == MAP_P1 || setsel == MAP_P2)
        hint = "Capture all 16 controls in sequence; release each control before the next";
    else if (setsel == RUMBLE)
        hint = psx_input_rumble_status(0);
    else if (setsel == ANALOG_MODE)
        hint = analog_mode ? "Both sticks enabled; L1 + R1 + Select toggles the emulated pad mode" : "Digital controller; choose Analog to enable both sticks";
    else if (setsel == RESOLUTION)
        hint = "Left/Right selects 1x, 2x, 3x or 4x internal 3D resolution for the next game start";
    else if (setsel == PERF_OVERLAY)
        hint = "Shows emulation speed over the game: Off, FPS, or Detailed. Left/Right changes; applies immediately";
    else if (setsel == EDGE_SMOOTHING)
        hint = "Optional edge-aware post-processing; independent of internal resolution and output filtering";
    put(footer, psx_input_error()[0] ? psx_input_error() : hint, 360, 1000, 22, 0);
}

static void draw(void)
{
    evas_object_color_set(bg, 12, 14, 20, 255);
    evas_object_move(bg, 0, 0);
    evas_object_resize(bg, sw, sh);
    evas_object_show(bg);
    if (page == GAME) {
        char hint[256];
        evas_object_hide(heading);
        hide_settings();
        for (int i = 0; i < ROWS; ++i) evas_object_hide(rows[i]);
        evas_object_hide(cursor);
        snprintf(hint, sizeof hint,
            "P1: %.55s  |  P2: %.55s  |  Remote: arrows, Z Cross, X Circle, Enter Start  |  Back stops",
            psx_input_status(0), psx_input_status(1));
        put(footer, hint, 100, 1010, 20, 0);
        pthread_mutex_lock(&frame_mutex);
        int have_frame = frame_sequence != 0;
        pthread_mutex_unlock(&frame_mutex);
        if (playing && have_frame) evas_object_show(image);
        else evas_object_hide(image);
        return;
    }

    evas_object_hide(image);
    evas_object_hide(perf_bg); evas_object_hide(perf_txt);
    evas_object_hide(footer);
    evas_object_hide(cursor);
    for (int i = 0; i < ROWS; ++i) evas_object_hide(rows[i]);
    if (page != SETTINGS) hide_settings();
    if (page == SETTINGS) {
        put(heading, "TizPSX settings", 360, 76, 48, 1);
        draw_settings();
        return;
    }
    const char *title = page == PICKER ? "Choose a user supplied file"
        : page == MESSAGE ? "TizPSX" : page == MAPPING ? "Map DualShock controls"
        : page == INPUT_TEST ? "Guided DualShock input test" : "TizPSX  -  select a game or Settings";
    put(heading, title, 360, 76, 48, 1);
    int line = 0;
    if (page == LIBRARY) {
        int count = psx_library_count();
        int total = count + 2;                       /* Exit, games, Settings */
        int top = libsel >= LIBRARY_VISIBLE ? libsel - LIBRARY_VISIBLE + 1 : 0;
        for (int i = top; i < total && line < LIBRARY_VISIBLE; ++i, ++line) {
            const psx_library_item *it = i >= 1 && i <= count ? psx_library_get(i - 1) : 0;
            put(rows[line], i == 0 ? "Exit TizPSX" : it ? it->name : "Settings and controller setup",
                360, 230 + line * 52, 28, i == libsel);
            if (i == libsel) {
                evas_object_color_set(cursor, 120, 200, 255, 255);
                evas_object_move(cursor, 330, 236 + line * 52);
                evas_object_resize(cursor, 8, 40);
                evas_object_show(cursor);
            }
        }
        put(footer, libsel == 0 ? "Cross/OK closes TizPSX   Start: Settings   Circle/Back exits"
                                : "Up/Down move   Cross/OK play or select   Start: Settings   Circle/Back exits",
            360, 1000, 22, 0);
    } else if (page == PICKER) {
        int top = picksel >= LIBRARY_VISIBLE ? picksel - LIBRARY_VISIBLE + 1 : 0;
        for (int i = top; i < psx_library_count() && line < LIBRARY_VISIBLE; ++i, ++line) {
            const psx_library_item *it = psx_library_get(i);
            if (it) put(rows[line], it->name, 360, 230 + line * 52, 27, i == picksel);
        }
        put(footer, "Choose a BIOS file from app data or mounted USB storage", 360, 1000, 22, 0);
    } else if (page == MAPPING) {
        char label[128], assigned[160], detail[220];
        psx_input_device_label((unsigned)mapping_player, assigned, sizeof assigned);
        put(rows[0], assigned, 390, 210, 28, 0);
        if (mapping_step < PSX_INPUT_BINDINGS) {
            unsigned binding = psx_input_mapping_binding((unsigned)mapping_step);
            psx_input_binding_label((unsigned)mapping_player, binding, label, sizeof label);
            snprintf(detail, sizeof detail, "%u/%u  Press %s  [%s]",
                     (unsigned)mapping_step + 1, PSX_INPUT_BINDINGS,
                     psx_input_binding_name(binding), label);
            put(rows[1], detail, 390, 270, 28, 1);
            put(rows[2], psx_input_capture_status((unsigned)mapping_player), 390, 325, 24, 0);
            put(rows[3], "Release the gamepad, then press the named button.", 390, 380, 24, 0);
            put(footer, "TV remote OK skips   TV remote Back cancels   30-second timeout",
                360, 1000, 22, 0);
        } else {
            put(rows[1], "All DualShock controls mapped.", 390, 270, 30, 1);
            put(footer, "Cross/OK or Circle/Back returns to Settings", 360, 1000, 22, 0);
        }
    } else if (page == INPUT_TEST) {
        char status[180], device[160];
        psx_input_test_label(status, sizeof status);
        psx_input_device_label(0, device, sizeof device);
        put(rows[0], status, 390, 210, 28, 1);
        put(rows[1], device, 390, 270, 24, 0);
        put(rows[2], psx_input_capture_status(0), 390, 320, 24, 0);
        put(rows[3], "Press and release each control in order; mappings are unchanged.", 390, 370, 22, 0);
        put(footer, "TV remote OK skips   TV remote Back returns to Settings", 360, 1000, 22, 0);
    } else {
        put(rows[0], notice, 390, 220, 28, 0);
        put(footer, "Cross/OK or Circle/Back returns", 360, 1000, 22, 0);
    }
}

static void show_message(const char *text)
{
    snprintf(notice, sizeof notice, "%s", text ? text : "Unknown error");
    page = MESSAGE;
}

static void audio_callback(audio_out_h handle, unsigned bytes, void *data)
{
    (void)data;
    static int16_t out[8192];
    unsigned frames = bytes / 4;
    if (frames > sizeof out / 4) frames = sizeof out / 4;
    unsigned r = __atomic_load_n(&audio_read, __ATOMIC_RELAXED);
    unsigned w = __atomic_load_n(&audio_write, __ATOMIC_ACQUIRE);
    unsigned available = w - r;
    if (available > AUDIO_RING_FRAMES) available = AUDIO_RING_FRAMES;
    unsigned take = frames < available ? frames : available;
    for (unsigned i = 0; i < take; ++i) {
        int l = audio_ring[((r + i) & (AUDIO_RING_FRAMES - 1u)) * 2u];
        int rr = audio_ring[((r + i) & (AUDIO_RING_FRAMES - 1u)) * 2u + 1u];
        if (mute) l = rr = 0;
        else { l = l * volume / 100; rr = rr * volume / 100; }
        out[i * 2] = (int16_t)l;
        out[i * 2 + 1] = (int16_t)rr;
    }
    for (unsigned i = take; i < frames; ++i) out[i * 2] = out[i * 2 + 1] = 0;
    __atomic_store_n(&audio_read, r + take, __ATOMIC_RELEASE);
    audio_out_write(handle, out, frames * 4u);
}

static void audio_open(void)
{
    audio_handle = 0;
    __atomic_store_n(&audio_read, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&audio_write, 0, __ATOMIC_RELAXED);
    if (audio_out_create_new(PSX_CORE_AUDIO_RATE, AUDIO_CHANNEL_STEREO,
                             AUDIO_SAMPLE_TYPE_S16LE, &audio_handle) || !audio_handle)
        return;
    if (audio_out_set_stream_cb(audio_handle, audio_callback, 0) || audio_out_prepare(audio_handle)) {
        audio_out_destroy(audio_handle);
        audio_handle = 0;
    }
}

static void audio_close(void)
{
    if (audio_handle) {
        audio_out_unprepare(audio_handle);
        audio_out_destroy(audio_handle);
        audio_handle = 0;
    }
    __atomic_store_n(&audio_read, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&audio_write, 0, __ATOMIC_RELAXED);
}

static void queue_audio(void)
{
    unsigned r = __atomic_load_n(&audio_read, __ATOMIC_ACQUIRE);
    unsigned w = __atomic_load_n(&audio_write, __ATOMIC_RELAXED);
    unsigned frames = audio.frames > PSX_CORE_AUDIO_MAX_FRAMES
        ? PSX_CORE_AUDIO_MAX_FRAMES : (unsigned)audio.frames;
    unsigned accepted = frames;
    if (accepted > AUDIO_RING_FRAMES - (w - r)) accepted = AUDIO_RING_FRAMES - (w - r);
    for (unsigned i = 0; i < accepted; ++i) {
        audio_ring[((w + i) & (AUDIO_RING_FRAMES - 1u)) * 2u] = audio.samples[i * 2u];
        audio_ring[((w + i) & (AUDIO_RING_FRAMES - 1u)) * 2u + 1u] = audio.samples[i * 2u + 1u];
    }
    __atomic_store_n(&audio_write, w + accepted, __ATOMIC_RELEASE);
}

static void publish_video(void)
{
    if (!video || !video->valid || !video->width || !video->height ||
        video->width > PSX_CORE_MAX_WIDTH || video->height > PSX_CORE_MAX_HEIGHT ||
        video->pitch_pixels < video->width || video->pitch_pixels > PSX_CORE_MAX_WIDTH ||
        (size_t)(video->height - 1u) * video->pitch_pixels + video->width > PIXEL_CAP) return;

    pthread_mutex_lock(&frame_mutex);
    unsigned back = 1u - published_index;
    pthread_mutex_unlock(&frame_mutex);
    uint32_t *dst = argb_frames[back];
    psx_convert_rgb565(video->pixels, video->pitch_pixels, video->width, video->height, dst);
    if (antialiasing && video->width >= 3 && video->height >= 3)
        psx_edge_smooth(dst, aa_scratch, aa_luma, video->width, video->height);
    pthread_mutex_lock(&frame_mutex);
    frame_width = video->width;
    frame_height = video->height;
    published_index = back;
    ++frame_sequence;
    pthread_mutex_unlock(&frame_mutex);
}

static void *emulation_worker(void *unused)
{
    (void)unused;
    unsigned rate = psx_core_frame_rate_millihz ? psx_core_frame_rate_millihz() : 60000u;
    if (rate < 10000u || rate > 120000u) rate = 60000u;
    unsigned long long period = 1000000000000ULL / rate;
    unsigned long long next = now_ns();
    perf_stats overlay;
    perf_start_ns = next;
    perf_reset(&overlay, next);
    perf_native_x100 = rate / 10u;

    while (!__atomic_load_n(&worker_stop, __ATOMIC_ACQUIRE)) {
        unsigned long long now = now_ns();
        if (now > next + period * WORKER_MAX_LAG) {
            next = now - period;
            ++perf_late;
        }
        unsigned ran = 0;
        while (now >= next && ran < WORKER_MAX_CATCHUP &&
               !__atomic_load_n(&worker_stop, __ATOMIC_ACQUIRE)) {
            /* Retain video pixels/metadata for libretro duplicate frames. */
            audio.frames = 0;
            unsigned long long start = now_ns();
            uint32_t input = __atomic_load_n(&worker_buttons[0], __ATOMIC_ACQUIRE) |
                __atomic_exchange_n(&buttons, 0, __ATOMIC_ACQ_REL);
            uint32_t input2 = __atomic_load_n(&worker_buttons[1], __ATOMIC_ACQUIRE);
            int16_t analog[2][4];
            for (unsigned p = 0; p < 2; ++p) for (unsigned a = 0; a < 4; ++a)
                analog[p][a] = __atomic_load_n(&worker_analog[p][a], __ATOMIC_ACQUIRE);
            int rc = psx_core_run(input, input2, analog[0], analog[1], video, &audio);
            unsigned long long after_core = now_ns();
            perf_core_ns += after_core - start;
            if (after_core - start > perf_core_max_ns) perf_core_max_ns = after_core - start;
            if (rc) {
                pthread_mutex_lock(&frame_mutex);
                snprintf(worker_error, sizeof worker_error, "%s",
                    psx_core_error ? psx_core_error() : "PlayStation core stopped.");
                pthread_mutex_unlock(&frame_mutex);
                __atomic_store_n(&worker_failed, 1, __ATOMIC_RELEASE);
                return 0;
            }
            queue_audio();
            unsigned long long convert_start = now_ns();
            publish_video();
            unsigned long long frame_done = now_ns();
            perf_convert_ns += frame_done - convert_start;
            if (__atomic_load_n(&perf_mode, __ATOMIC_RELAXED) != PERF_OVERLAY_OFF &&
                perf_frame(&overlay, frame_done, frame_done - start, frame_done > next + period)) {
                pthread_mutex_lock(&frame_mutex);
                perf_pub = overlay;
                ++perf_pub_seq;
                pthread_mutex_unlock(&frame_mutex);
            }
            ++perf_frames;
            ++ran;
            next += period;
            now = now_ns();
        }
        if (!ran) {
            unsigned long long remaining = next > now ? next - now : 0;
            if (remaining > 100000ULL) {
                struct timespec delay;
                delay.tv_sec = (long)(remaining / 1000000000ULL);
                delay.tv_nsec = (long)(remaining % 1000000000ULL);
                nanosleep(&delay, 0);
            }
        }
        now = now_ns();
        if (now >= perf_start_ns && now - perf_start_ns >= 10000000000ULL) {
            char line[320];
            unsigned long long span = now - perf_start_ns;
            unsigned ui = __atomic_exchange_n(&perf_ui_ticks, 0, __ATOMIC_RELAXED);
            unsigned presented = __atomic_exchange_n(&perf_presented, 0, __ATOMIC_RELAXED);
            unsigned fps_x100 = span ? (unsigned)(perf_frames * 100000000000ULL / span) : 0;
            snprintf(line, sizeof line,
                "perf fps_x100=%u ui_ticks=%u presented=%u late=%llu core_avg_us=%llu core_max_us=%llu convert_avg_us=%llu",
                fps_x100, ui, presented, perf_late,
                perf_frames ? perf_core_ns / perf_frames / 1000ULL : 0,
                perf_core_max_ns / 1000ULL,
                perf_frames ? perf_convert_ns / perf_frames / 1000ULL : 0);
            diag(line);
            perf_start_ns = now;
            perf_frames = perf_core_ns = perf_convert_ns = perf_core_max_ns = perf_late = 0;
        }
    }
    return 0;
}

static int start_worker(void)
{
    if (worker_started) return 1;
    __atomic_store_n(&worker_stop, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&worker_failed, 0, __ATOMIC_RELEASE);
    pthread_mutex_lock(&frame_mutex);
    frame_sequence = last_presented_sequence = 0;
    perf_pub_seq = perf_shown_seq = 0;
    memset(&perf_pub, 0, sizeof perf_pub);
    pthread_mutex_unlock(&frame_mutex);
    perf_frames = perf_core_ns = perf_convert_ns = perf_core_max_ns = perf_late = 0;
    perf_ui_ticks = perf_presented = 0;
    if (pthread_create(&worker_thread, 0, emulation_worker, 0) != 0) {
        diag("emulation worker start failed");
        return 0;
    }
    worker_started = 1;
    return 1;
}

static void stop_worker(void)
{
    if (!worker_started) return;
    __atomic_store_n(&worker_stop, 1, __ATOMIC_RELEASE);
    pthread_join(worker_thread, 0);
    worker_started = 0;
}

/* Performance overlay (emulator-framework/docs/overlay.md): top-right, drawn
 * only while a game is on screen. The worker publishes one snapshot per
 * second; the UI thread reformats the text only when a new one arrives. */
static void perf_overlay_update(void)
{
    perf_stats snap;
    char text[200];
    if (perf_mode == PERF_OVERLAY_OFF || page != GAME || !playing) {
        evas_object_hide(perf_bg); evas_object_hide(perf_txt);
        perf_shown_seq = 0;
        return;
    }
    pthread_mutex_lock(&frame_mutex);
    unsigned seq = perf_pub_seq;
    snap = perf_pub;
    pthread_mutex_unlock(&frame_mutex);
    if (seq == perf_shown_seq) return;
    perf_shown_seq = seq;
    if (perf_format(&snap, (perf_overlay_mode)perf_mode, perf_native_x100, 0, text, sizeof text) <= 0) return;
    evas_object_text_font_set(perf_txt, "Sans", 28 * sw / 1920);
    evas_object_text_text_set(perf_txt, text);
    int w = evas_object_text_horiz_advance_get(perf_txt);
    int x = sw - 48 * sw / 1920 - w;
    evas_object_move(perf_bg, x - 10, 38 * sh / 1080);
    evas_object_resize(perf_bg, w + 20, 48 * sh / 1080);
    evas_object_move(perf_txt, x, 44 * sh / 1080);
    evas_object_show(perf_bg); evas_object_show(perf_txt);
}

static void present_latest_frame(void)
{
    unsigned width, height, sequence;
    pthread_mutex_lock(&frame_mutex);
    sequence = frame_sequence;
    if (sequence == last_presented_sequence) {
        pthread_mutex_unlock(&frame_mutex);
        return;
    }
    width = frame_width;
    height = frame_height;
    if (!argb_frames || !argb_display || !width || !height || width > PSX_CORE_MAX_WIDTH ||
        height > PSX_CORE_MAX_HEIGHT || published_index >= 2u ||
        (size_t)width * height > PIXEL_CAP) {
        pthread_mutex_unlock(&frame_mutex);
        return;
    }
    memcpy(argb_display, argb_frames[published_index], (size_t)width * height * sizeof(uint32_t));
    last_presented_sequence = sequence;
    pthread_mutex_unlock(&frame_mutex);

    if (!width || !height) return;
    int dw, dh;
    if (integer) {
        int scale = sw / (int)width;
        if (sh / (int)height < scale) scale = sh / (int)height;
        if (scale < 1) scale = 1;
        dw = (int)width * scale;
        dh = (int)height * scale;
    } else {
        double sx = (double)sw / width, sy = (double)sh / height;
        double scale = sx < sy ? sx : sy;
        dw = (int)(width * scale);
        dh = (int)(height * scale);
    }
    evas_object_move(image, (sw - dw) / 2, (sh - dh) / 2);
    evas_object_resize(image, dw, dh);
    evas_object_image_smooth_scale_set(image, filter);
    evas_object_image_size_set(image, (int)width, (int)height);
    evas_object_image_data_set(image, argb_display);
    evas_object_image_data_update_add(image, 0, 0, (int)width, (int)height);
    evas_object_show(image);
    __atomic_add_fetch(&perf_presented, 1, __ATOMIC_RELAXED);
}

static void audio_and_core_close(void)
{
    stop_worker();
    audio_close();
    if (psx_core_deinit) psx_core_deinit();
    frame_storage_close();
    playing = 0;
    psx_input_rumble_stop();
    __atomic_store_n(&worker_buttons[0], 0, __ATOMIC_RELEASE);
    __atomic_store_n(&worker_buttons[1], 0, __ATOMIC_RELEASE);
    pthread_mutex_lock(&frame_mutex);
    frame_sequence = last_presented_sequence = 0;
    pthread_mutex_unlock(&frame_mutex);
}

static void start_game(const char *path)
{
    if (!bios[0]) { show_message("Select a user supplied PSX BIOS in Settings first."); return; }
    if (!psx_core_init || !psx_core_run) { show_message("No PSX core is linked into this build."); return; }
    diag("game-start: initializing supplied content");
    int rc = psx_core_init(bios, path, (unsigned)resolution_multiplier);
    if (rc) {
        const char *error = psx_core_error ? psx_core_error() : "PlayStation core initialization failed.";
        diag(error);
        show_message(error);
        return;
    }
    if (!frame_storage_open()) {
        if (psx_core_deinit) psx_core_deinit();
        diag("game-start: unable to allocate frame buffers");
        show_message("Not enough memory to allocate the video buffers.");
        return;
    }
    if (psx_core_set_analog_mode) psx_core_set_analog_mode(analog_mode);
    /* Clear once for a new game, before the worker can receive a dupe. */
    __atomic_store_n(&buttons, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&worker_buttons[0], 0, __ATOMIC_RELEASE);
    __atomic_store_n(&worker_buttons[1], 0, __ATOMIC_RELEASE);
    audio_open();
    playing = 1;
    page = GAME;
    if (!start_worker()) {
        audio_close();
        if (psx_core_deinit) psx_core_deinit();
        frame_storage_close();
        playing = 0;
        diag("game-start: worker thread creation failed");
        show_message("Could not start the emulation worker thread.");
    } else diag("game-start: worker running");
}

static Eina_Bool autostart_game(void *data)
{
    (void)data;
    if (!autostart[0]) return 0;
    char path[PATH_CAP];
    snprintf(path, sizeof path, "%s", autostart);
    autostart[0] = '\0';
    save_settings(); /* Consume the one-shot request before attempting the load. */
    diag("game-start: consuming one-shot launch request");
    start_game(path);
    draw();
    return 0;
}

static void mapping_capture_step(void)
{
    if (mapping_step >= PSX_INPUT_BINDINGS) {
        mapping_active = 0;
        page = SETTINGS;
        return;
    }
    psx_input_capture((unsigned)mapping_player,
        psx_input_mapping_binding((unsigned)mapping_step), now_ns());
    mapping_active = psx_input_capturing();
}

static void mapping_start(unsigned player)
{
    mapping_player = player < 2 ? (int)player : 0;
    mapping_step = 0;
    page = MAPPING;
    mapping_capture_step();
    if (!mapping_active) {
        page = SETTINGS;
        show_message("Assign a connected controller to this player before mapping its buttons.");
    }
}

static void back_action(void)
{
    if (page == SETTINGS) page = LIBRARY;
    else if (page == PICKER) page = SETTINGS;
    else if (page == MAPPING) {
        psx_input_cancel_capture();
        mapping_active = 0;
        page = SETTINGS;
    } else if (page == INPUT_TEST) {
        psx_input_test_cancel();
        page = SETTINGS;
    } else if (page == LIBRARY) ui_app_exit();
    else page = LIBRARY;
    draw();
}

static void settings_adjust(int direction)
{
    if (setsel == P1_DEVICE || setsel == P2_DEVICE) {
        psx_input_cycle_device(setsel == P1_DEVICE ? 0 : 1, direction);
    } else if (setsel == EXCLUSIVE) {
        psx_input_toggle_exclusive();
    } else if (setsel == RUMBLE) {
        psx_input_set_rumble_enabled(!psx_input_rumble_enabled());
    } else if (setsel == ANALOG_MODE) {
        analog_mode = !analog_mode;
        save_settings();
    } else if (setsel == VOLUME) {
        volume += direction * 5;
        if (volume < 0) volume = 0;
        if (volume > 100) volume = 100;
        save_settings();
    } else if (setsel == MUTE) {
        mute = !mute;
        save_settings();
    } else if (setsel == SCREEN) {
        integer = !integer;
        save_settings();
    } else if (setsel == RESOLUTION) {
        resolution_multiplier = (resolution_multiplier - 1 + (direction > 0 ? 1 : 3)) % 4 + 1;
        save_settings();
    } else if (setsel == EDGE_SMOOTHING) {
        antialiasing = !antialiasing;
        save_settings();
    } else if (setsel == FILTER) {
        filter = !filter;
        save_settings();
    } else if (setsel == PERF_OVERLAY) {
        perf_mode = (perf_mode + direction + PERF_OVERLAY_MODES) % PERF_OVERLAY_MODES;
        save_settings();
    }
    draw();
}

static void activate(void)
{
    if (page == LIBRARY) {
        if (libsel == 0) { ui_app_exit(); return; }
        if (libsel == psx_library_count() + 1) { page = SETTINGS; setsel = P1_DEVICE; }
        else {
            const psx_library_item *it = psx_library_get(libsel - 1);
            if (it) start_game(it->path);
        }
    } else if (page == SETTINGS) {
        if (setsel == P1_DEVICE || setsel == P2_DEVICE) {
            psx_input_cycle_device(setsel == P1_DEVICE ? 0 : 1, 1);
        } else if (setsel == EXCLUSIVE) {
            psx_input_toggle_exclusive();
        } else if (setsel == MAP_P1 || setsel == MAP_P2) {
            mapping_start(setsel == MAP_P1 ? 0 : 1);
        } else if (setsel == INPUT_DIAG) {
            input_test_player = 0;
            psx_input_test_begin(0);
            page = INPUT_TEST;
        } else if (setsel == RUMBLE) {
            psx_input_set_rumble_enabled(!psx_input_rumble_enabled());
        } else if (setsel == ANALOG_MODE) {
            analog_mode = !analog_mode;
            save_settings();
        } else if (setsel == BIOS) { page = PICKER; picksel = 0; }
        else if (setsel == MUTE) { mute = !mute; save_settings(); }
        else if (setsel == PERF_OVERLAY) settings_adjust(1);
        else if (setsel == RESCAN) { psx_library_scan(); libsel = psx_library_count() > 0 ? 1 : psx_library_count() + 1; show_message("Library rescan complete."); }
        else if (setsel == BACK) page = LIBRARY;
        else if (setsel == EXIT_APP) ui_app_exit();
    } else if (page == PICKER) {
        const psx_library_item *it = psx_library_get(picksel);
        if (it && psx_library_is_bios_candidate(it)) {
            snprintf(bios, sizeof bios, "%s", it->path);
            save_settings();
            page = SETTINGS;
        } else show_message("BIOS selection requires a user supplied .bin file.");
    } else if (page == GAME) {
        audio_and_core_close();
        page = LIBRARY;
    } else if (page == MAPPING) {
        if (mapping_step >= PSX_INPUT_BINDINGS) page = SETTINGS;
        else {
            psx_input_cancel_capture();
            ++mapping_step;
            mapping_capture_step();
        }
    } else if (page == INPUT_TEST) {
        psx_input_test_skip();
    } else page = LIBRARY;
    draw();
}

static void queue_button(uint32_t button)
{
    __atomic_fetch_or(&buttons, button, __ATOMIC_RELEASE);
}

static void menu_up(void)
{
    if (page == LIBRARY && libsel > 0) --libsel;
    else if (page == SETTINGS && setsel > 0) --setsel;
    else if (page == PICKER && picksel > 0) --picksel;
    draw();
}
static void menu_down(void)
{
    if (page == LIBRARY && libsel < psx_library_count() + 1) ++libsel;
    else if (page == SETTINGS && setsel < SET_COUNT - 1) ++setsel;
    else if (page == PICKER && picksel < psx_library_count() - 1) ++picksel;
    draw();
}
static void menu_left(void) { if (page == SETTINGS) settings_adjust(-1); }
static void menu_right(void) { if (page == SETTINGS) settings_adjust(1); }
static void menu_select(void) { activate(); }
static void menu_back(void)
{
    if (page == GAME) activate();
    else if (page == MAPPING || page == INPUT_TEST || page == SETTINGS || page == PICKER) back_action();
    else if (page == LIBRARY) ui_app_exit();
    else { page = LIBRARY; draw(); }
}

static Eina_Bool key(void *data, int type, void *event)
{
    (void)data; (void)type;
    key_event_t *k = (key_event_t *)event;
    const char *name = k && k->keyname ? k->keyname : "";
    unsigned long long event_time = now_ns();
    psx_input_poll(event_time);
    if (psx_input_recent_activity(event_time)) return 1;
    if (page == GAME) {
        if (!__builtin_strcmp(name, "Escape") || !__builtin_strcmp(name, "XF86Back")) {
            menu_back();
            return 1;
        }
        if (!__builtin_strcmp(name, "Up")) queue_button(PSX_PAD_UP);
        else if (!__builtin_strcmp(name, "Down")) queue_button(PSX_PAD_DOWN);
        else if (!__builtin_strcmp(name, "Left")) queue_button(PSX_PAD_LEFT);
        else if (!__builtin_strcmp(name, "Right")) queue_button(PSX_PAD_RIGHT);
        else if (!__builtin_strcmp(name, "z")) queue_button(PSX_PAD_B); /* Cross */
        else if (!__builtin_strcmp(name, "x")) queue_button(PSX_PAD_A); /* Circle */
        else if (!__builtin_strcmp(name, "Return")) queue_button(PSX_PAD_START);
        return 1;
    }
    if (!__builtin_strcmp(name, "Up")) menu_up();
    else if (!__builtin_strcmp(name, "Down")) menu_down();
    else if (!__builtin_strcmp(name, "Left")) menu_left();
    else if (!__builtin_strcmp(name, "Right")) menu_right();
    else if (!__builtin_strcmp(name, "Return") || !__builtin_strcmp(name, "KP_Enter") || !__builtin_strcmp(name, "z")) menu_select();
    else if (!__builtin_strcmp(name, "Escape") || !__builtin_strcmp(name, "XF86Back") || !__builtin_strcmp(name, "x")) menu_back();
    return 1;
}

static Eina_Bool tick(void *data)
{
    (void)data;
    psx_input_poll(now_ns());
    uint32_t pad = psx_input_buttons(0);
    static uint32_t previous_pad;
    uint32_t pressed = pad & ~previous_pad;
    previous_pad = pad;

    if (page == GAME && playing) {
        uint32_t held0 = psx_input_buttons(0);
        uint32_t held1 = psx_input_buttons(1);
        __atomic_store_n(&worker_buttons[0], held0, __ATOMIC_RELEASE);
        __atomic_store_n(&worker_buttons[1], held1, __ATOMIC_RELEASE);
        for (unsigned p = 0; p < 2; ++p) for (unsigned a = 0; a < 4; ++a)
            __atomic_store_n(&worker_analog[p][a], psx_input_analog(p, a), __ATOMIC_RELEASE);
        __atomic_add_fetch(&perf_ui_ticks, 1, __ATOMIC_RELAXED);
        present_latest_frame();
        perf_overlay_update();
        if (__atomic_load_n(&worker_failed, __ATOMIC_ACQUIRE)) {
            char error[sizeof worker_error];
            stop_worker();
            pthread_mutex_lock(&frame_mutex);
            snprintf(error, sizeof error, "%s", worker_error);
            pthread_mutex_unlock(&frame_mutex);
            audio_close();
            if (psx_core_deinit) psx_core_deinit();
            frame_storage_close();
            psx_input_rumble_stop();
            playing = 0;
            show_message(error[0] ? error : "PlayStation core stopped.");
            draw();
        }
    } else if (pressed) {
        if (pressed & PSX_PAD_UP) menu_up();
        else if (pressed & PSX_PAD_DOWN) menu_down();
        else if (pressed & PSX_PAD_LEFT) menu_left();
        else if (pressed & PSX_PAD_RIGHT) menu_right();
        else if (pressed & PSX_PAD_START && page == LIBRARY) { page = SETTINGS; setsel = P1_DEVICE; draw(); }
        else if (pressed & PSX_PAD_B) menu_select(); /* Cross */
        else if (pressed & PSX_PAD_A) menu_back(); /* Circle */
        else if (page == MAPPING && (pressed & PSX_PAD_SELECT)) psx_input_test_skip();
    }
    if (page == MAPPING && psx_input_capture_succeeded()) {
        ++mapping_step;
        mapping_capture_step();
        draw();
    } else if (page == INPUT_TEST && psx_input_changed()) draw();
    else if ((page == SETTINGS || page == MAPPING) && psx_input_changed()) draw();
    return 1;
}

static void create(void *data)
{
    (void)data;
    diag("ui-create: begin");
    Evas_Object *window = elm_win_util_standard_add("tizpsx", "TizPSX");
    if (!window) { diag("ui-create: window creation failed"); return; }
    elm_win_fullscreen_set(window, 1);
    /* Match the NES/GB launch path: the TV's Evas output reports 1x1 until
     * the native window has explicit geometry. Size it before laying out UI. */
    evas_object_resize(window, 1920, 1080);
    sw = 1920;
    sh = 1080;
    evas = evas_object_evas_get(window);
    char message[160];
    snprintf(message, sizeof message, "ui-create: canvas=%dx%d", sw, sh);
    diag(message);
    bg = evas_object_rectangle_add(evas);
    heading = evas_object_text_add(evas);
    footer = evas_object_text_add(evas);
    cursor = evas_object_rectangle_add(evas);
    image = evas_object_image_filled_add(evas);
    evas_object_image_alpha_set(image, 0);
    for (int i = 0; i < SECTION_COUNT; ++i) {
        set_heads[i] = evas_object_text_add(evas);
        set_rules[i] = evas_object_rectangle_add(evas);
    }
    for (int i = 0; i < ROWS; ++i) rows[i] = evas_object_text_add(evas);
    /* Created last: stays above the game image. */
    perf_bg = evas_object_rectangle_add(evas);
    evas_object_color_set(perf_bg, 0, 0, 0, 160);
    perf_txt = evas_object_text_add(evas);
    evas_object_color_set(perf_txt, 255, 255, 255, 255);
    evas_object_hide(perf_bg); evas_object_hide(perf_txt);
    if (!bg || !heading || !footer || !cursor || !image || !rows[0] ||
        !set_heads[0] || !set_rules[0]) {
        diag("ui-create: Evas object allocation failed");
        return;
    }
    elm_win_resize_object_add(window, bg);
    load_settings();
    diag(bios[0] ? "ui-create: settings loaded with BIOS" : "ui-create: settings loaded without BIOS");
    psx_library_scan();
    snprintf(message, sizeof message, "ui-create: library items=%d", psx_library_count());
    diag(message);
    libsel = psx_library_count() > 0 ? 1 : psx_library_count() + 1;   /* first game, or Settings when empty; never Exit */
    relocate_bios_after_remount();
    psx_input_init(TIZPSX_DATA_DIR "/controller.ini");
    draw();
    evas_object_show(window);
    elm_object_focus_set(window, 1);
    diag("ui-create: initial draw and window show complete");
    ecore_event_handler_add(ECORE_EVENT_KEY_DOWN, key, 0);
    ecore_timer_add(1.0 / 60.0, tick, 0);
    if (autostart[0]) ecore_timer_add(0.5, autostart_game, 0);
}

static void terminate(void *data)
{
    (void)data;
    stop_worker();
    audio_close();
    if (playing && psx_core_deinit) psx_core_deinit();
    frame_storage_close();
    playing = 0;
    psx_input_shutdown();
    ui_app_exit();
}

static void pause_app(void *data)
{
    (void)data;
    if (page == MAPPING || page == INPUT_TEST) page = SETTINGS;
    if (playing) {
        stop_worker();
        audio_close();
        __atomic_store_n(&worker_buttons[0], 0, __ATOMIC_RELEASE);
        __atomic_store_n(&worker_buttons[1], 0, __ATOMIC_RELEASE);
    }
    psx_input_rumble_stop();
    psx_input_pause_capture();
}

static void resume_app(void *data)
{
    (void)data;
    psx_input_resume_capture();
    if (playing) {
        audio_open();
        if (!start_worker()) {
            audio_close();
            if (psx_core_deinit) psx_core_deinit();
            psx_input_shutdown();
            playing = 0;
            show_message("Could not restart the emulation worker.");
            draw();
        }
    }
    draw();
}

int main(int argc, char **argv)
{
    callbacks_t cb = { create, terminate, pause_app, resume_app, 0 };
    return ui_app_main(argc, argv, &cb, 0);
}
