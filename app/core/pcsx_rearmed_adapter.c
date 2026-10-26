#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include "../psx_core.h"
#include "../input.h"

#include <libretro.h>

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef TIZPSX_DATA_DIR
#define TIZPSX_DATA_DIR "/opt/usr/home/owner/apps_rw/org.pablo.tizpsx/data"
#endif

static char core_error[256];
static char temp_dir[] = "/tmp/tizpsx-core-XXXXXX";
static char save_dir[512] = TIZPSX_DATA_DIR "/saves";
static int temp_dir_created;
static int core_initialized;
static int game_loaded;
static unsigned frame_rate_millihz = 60000;
static unsigned requested_resolution_multiplier = 1;
static psx_video_frame_t *active_video;
static psx_audio_frame_t *active_audio;
static const uint32_t *active_buttons;
static int16_t active_analog[2][4];
extern void psx_diag(const char *line);

/* Private PCSX ReARMed frontend device id: RETRO_DEVICE_SUBCLASS(ANALOG, 1). */
#define RETRO_DEVICE_PSE_DUALSHOCK 0x205u

static void set_error(const char *fmt, ...);

/* Libretro entry points implemented by the vendored PCSX ReARMed frontend. */
void retro_init(void);
void retro_deinit(void);
bool retro_load_game(const struct retro_game_info *info);
void retro_unload_game(void);
void retro_run(void);
void retro_set_environment(retro_environment_t cb);
void retro_set_video_refresh(retro_video_refresh_t cb);
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb);
void retro_set_input_poll(retro_input_poll_t cb);
void retro_set_input_state(retro_input_state_t cb);
void retro_set_controller_port_device(unsigned port, unsigned device);
void retro_get_system_av_info(struct retro_system_av_info *info);

/* Bundled GPU_NEON renderer extension, selected before core initialization. */
extern void GPUsetResolutionScale(unsigned multiplier);

static bool environment(unsigned command, void *data);
static void video_refresh(const void *data, unsigned width, unsigned height,
                          size_t pitch);
static size_t audio_batch(const int16_t *data, size_t frames);
static int16_t input_state(unsigned port, unsigned device, unsigned index,
                           unsigned id);
static bool rumble_state(unsigned port, enum retro_rumble_effect effect, uint16_t strength);
static void input_poll(void) { }

static int regular_file(const char *path)
{
    struct stat st;
    return path && path[0] && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int supported_bios(const char *path)
{
    unsigned char prefix[5];
    char signature[13];
    FILE *fp;
    struct stat st;
    int ok = 0;
    if (!path || stat(path, &st) != 0 || st.st_size != 512 * 1024)
        return 0;
    fp = fopen(path, "rb");
    if (!fp)
        return 0;
    if (fread(prefix, 1, sizeof(prefix), fp) == sizeof(prefix) &&
        fseek(fp, 0x12c, SEEK_SET) == 0 &&
        fread(signature, 1, sizeof(signature), fp) == sizeof(signature)) {
        ok = (!memcmp(prefix + 1, "\x00\x08\x3c\x3f", 4) ||
              !memcmp(signature, "PS compatible", sizeof(signature)));
    }
    fclose(fp);
    return ok;
}

int psx_core_bios_supported(const char *path)
{
    return regular_file(path) && supported_bios(path);
}

static int supported_content(const char *path)
{
    const char *ext = strrchr(path, '.');
    static const char *const allowed[] = {
        ".cue", ".bin", ".iso", ".img", ".chd", ".pbp", ".m3u"
    };
    size_t i;
    if (!ext)
        return 0;
    for (i = 0; i < sizeof(allowed) / sizeof(allowed[0]); ++i) {
        size_t j;
        for (j = 0; ext[j] && allowed[i][j] &&
                    tolower((unsigned char)ext[j]) == allowed[i][j]; ++j) { }
        if (!ext[j] && !allowed[i][j])
            return 1;
    }
    return 0;
}

static void cleanup_temp_dir(void)
{
    char bios_alias[sizeof(temp_dir) + 32];
    if (!temp_dir_created)
        return;
    snprintf(bios_alias, sizeof(bios_alias), "%s/scph1001.bin", temp_dir);
    (void)unlink(bios_alias);
    (void)rmdir(temp_dir);
    temp_dir_created = 0;
    strcpy(temp_dir, "/tmp/tizpsx-core-XXXXXX");
}

static int ensure_save_dir(void)
{
    struct stat st;
    if (mkdir(TIZPSX_DATA_DIR, 0700) != 0 && errno != EEXIST) {
        set_error("Cannot create app data directory: %s", strerror(errno));
        return -1;
    }
    if (stat(TIZPSX_DATA_DIR, &st) != 0 || !S_ISDIR(st.st_mode)) {
        set_error("App data path is not a directory");
        return -1;
    }
    if (mkdir(save_dir, 0700) != 0 && errno != EEXIST) {
        set_error("Cannot create memory-card directory: %s", strerror(errno));
        return -1;
    }
    if (stat(save_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        set_error("Memory-card path is not a directory");
        return -1;
    }
    return 0;
}

int psx_core_init(const char *bios_path, const char *content_path,
                  unsigned resolution_multiplier)
{
    char bios_real[4096];
    char bios_alias[sizeof(temp_dir) + 32];
    struct retro_game_info game;

    psx_diag("core-init: cleanup previous session");
    psx_core_deinit();
    requested_resolution_multiplier = resolution_multiplier >= 1 && resolution_multiplier <= 4
        ? resolution_multiplier : 1;
    core_error[0] = '\0';
    if (!regular_file(bios_path)) {
        set_error("BIOS path is not a readable regular file");
        return -1;
    }
    if (!supported_bios(bios_path)) {
        set_error("Unsupported BIOS (PCSX ReARMed requires a recognized 512 KiB BIOS image)");
        return -1;
    }
    if (!regular_file(content_path)) {
        set_error("Content path is not a readable regular file");
        return -1;
    }
    if (!supported_content(content_path)) {
        set_error("Unsupported PSX image format (expected cue/bin/iso/img/chd/pbp/m3u)");
        return -1;
    }
    if (ensure_save_dir() != 0)
        return -1;
    psx_diag("core-init: BIOS and content validated");
    if (!realpath(bios_path, bios_real)) {
        set_error("Cannot resolve BIOS path: %s", strerror(errno));
        return -1;
    }
    if (!mkdtemp(temp_dir)) {
        set_error("Cannot create private core directory: %s", strerror(errno));
        strcpy(temp_dir, "/tmp/tizpsx-core-XXXXXX");
        return -1;
    }
    temp_dir_created = 1;
    snprintf(bios_alias, sizeof(bios_alias), "%s/scph1001.bin", temp_dir);
    if (symlink(bios_real, bios_alias) != 0) {
        set_error("Cannot expose supplied BIOS to core: %s", strerror(errno));
        cleanup_temp_dir();
        return -1;
    }

    retro_set_environment(environment);
    retro_set_video_refresh(video_refresh);
    retro_set_audio_sample_batch(audio_batch);
    retro_set_input_poll(input_poll);
    retro_set_input_state(input_state);
    GPUsetResolutionScale(requested_resolution_multiplier);
    psx_diag("core-init: retro_init begin");
    retro_init();
    core_initialized = 1;
    psx_diag("core-init: retro_init complete");

    memset(&game, 0, sizeof(game));
    game.path = content_path;
    psx_diag("core-init: retro_load_game begin");
    if (!retro_load_game(&game)) {
        char saved_error[sizeof(core_error)];
        if (!core_error[0])
            set_error("PCSX ReARMed rejected the supplied content or BIOS");
        memcpy(saved_error, core_error, sizeof(saved_error));
        psx_core_deinit();
        memcpy(core_error, saved_error, sizeof(core_error));
        return -1;
    }
    psx_diag("core-init: retro_load_game complete");
    retro_set_controller_port_device(0, RETRO_DEVICE_PSE_DUALSHOCK);
    retro_set_controller_port_device(1, RETRO_DEVICE_PSE_DUALSHOCK);
    {
        struct retro_system_av_info av;
        memset(&av, 0, sizeof av);
        retro_get_system_av_info(&av);
        if (av.timing.fps >= 45.0 && av.timing.fps <= 65.0)
            frame_rate_millihz = (unsigned)(av.timing.fps * 1000.0 + 0.5);
        else
            frame_rate_millihz = 60000;
    }
    game_loaded = 1;
    return 0;
}

int psx_core_run(uint32_t p1_buttons, uint32_t p2_buttons,
                 const int16_t p1_analog[4], const int16_t p2_analog[4],
                 psx_video_frame_t *video, psx_audio_frame_t *audio)
{
    uint32_t buttons[2];
    if (!core_initialized || !game_loaded || !video || !audio) {
        set_error("PCSX ReARMed is not initialized or output buffers are missing");
        return -1;
    }
    buttons[0] = p1_buttons;
    buttons[1] = p2_buttons;
    memcpy(active_analog[0], p1_analog, sizeof active_analog[0]);
    memcpy(active_analog[1], p2_analog, sizeof active_analog[1]);
    audio->frames = 0;
    active_video = video;
    active_audio = audio;
    active_buttons = buttons;
    core_error[0] = '\0';
    retro_run();
    active_video = NULL;
    active_audio = NULL;
    active_buttons = NULL;
    if (core_error[0] && !video->valid) {
        set_error("Core frame failed: %s", core_error);
        return -1;
    }
    return 0;
}

void psx_core_set_analog_mode(int enabled)
{
    unsigned device = enabled ? RETRO_DEVICE_PSE_DUALSHOCK : RETRO_DEVICE_JOYPAD;
    retro_set_controller_port_device(0, device);
    retro_set_controller_port_device(1, device);
}

void psx_core_deinit(void)
{
    active_video = NULL;
    active_audio = NULL;
    active_buttons = NULL;
    if (game_loaded) {
        retro_unload_game();
        game_loaded = 0;
    }
    if (core_initialized) {
        retro_deinit();
        core_initialized = 0;
    }
    cleanup_temp_dir();
}

unsigned psx_core_frame_rate_millihz(void)
{
    return frame_rate_millihz;
}

const char *psx_core_error(void)
{
    return core_error;
}

static void set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(core_error, sizeof(core_error), fmt, ap);
    va_end(ap);
}

static void core_log(enum retro_log_level level, const char *fmt, ...)
{
    va_list ap;
    (void)level;
    va_start(ap, fmt);
    vsnprintf(core_error, sizeof(core_error), fmt, ap);
    va_end(ap);
    core_error[sizeof(core_error) - 1] = '\0';
    psx_diag(core_error);
}

static bool environment(unsigned command, void *data)
{
    switch (command) {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        if (!data || !temp_dir_created)
            return false;
        *(const char **)data = temp_dir;
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        if (!data)
            return false;
        *(const char **)data = save_dir;
        return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        if (data) *(bool *)data = true;
        return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        if (data) *(bool *)data = false;
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE:
        if (data) {
            struct retro_variable *variable = data;
            if (!strcmp(variable->key, "pcsx_rearmed_analog_combo"))
                variable->value = "l1+r1+select";
            else if (!strcmp(variable->key, "pcsx_rearmed_vibration"))
                variable->value = "enabled";
            else if (!strcmp(variable->key, "pcsx_rearmed_neon_enhancement_enable"))
                variable->value = requested_resolution_multiplier > 1 ? "enabled" : "disabled";
            else
                variable->value = NULL;
            return variable->value != NULL;
        }
        return false;
    case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION:
        if (data) *(unsigned *)data = 0;
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return data && *(enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        if (data) ((struct retro_log_callback *)data)->log = core_log;
        return data != NULL;
    case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
        if (data) ((struct retro_rumble_interface *)data)->set_rumble_state = rumble_state;
        return data != NULL;
    default:
        return false;
    }
}

static void video_refresh(const void *data, unsigned width, unsigned height,
                          size_t pitch)
{
    unsigned y;
    if (!active_video || !data)
        return; /* Libretro frame duplication: retain the previous image. */
    if (!width || !height || width > PSX_CORE_MAX_WIDTH ||
        height > PSX_CORE_MAX_HEIGHT || pitch < (size_t)width * sizeof(uint16_t) ||
        pitch > (size_t)PSX_CORE_MAX_WIDTH * sizeof(uint16_t)) {
        set_error("PCSX ReARMed returned unsupported video geometry (%ux%u, pitch %zu)",
                  width, height, pitch);
        active_video->valid = 0;
        return;
    }
    for (y = 0; y < height; ++y)
        memcpy(active_video->pixels + (size_t)y * width,
               (const unsigned char *)data + (size_t)y * pitch,
               (size_t)width * sizeof(uint16_t));
    active_video->width = width;
    active_video->height = height;
    active_video->pitch_pixels = width;
    active_video->valid = 1;
}

static size_t audio_batch(const int16_t *data, size_t frames)
{
    size_t n;
    if (!active_audio || !data)
        return frames;
    n = PSX_CORE_AUDIO_MAX_FRAMES - active_audio->frames;
    if (n > frames)
        n = frames;
    memcpy(active_audio->samples + active_audio->frames * 2u,
           data, n * 2u * sizeof(int16_t));
    active_audio->frames += n;
    return n;
}

static int16_t input_state(unsigned port, unsigned device, unsigned index,
                           unsigned id)
{
    static const uint32_t masks[16] = {
        PSX_PAD_B, PSX_PAD_Y, PSX_PAD_SELECT, PSX_PAD_START,
        PSX_PAD_UP, PSX_PAD_DOWN, PSX_PAD_LEFT, PSX_PAD_RIGHT,
        PSX_PAD_A, PSX_PAD_X, PSX_PAD_L, PSX_PAD_R,
        PSX_PAD_L2, PSX_PAD_R2, PSX_PAD_L3, PSX_PAD_R3
    };
    if (port > 1)
        return 0;
    if (device == RETRO_DEVICE_ANALOG && index < 2 && id < 2)
        return active_analog[port][index * 2 + id];
    if (device != RETRO_DEVICE_JOYPAD || !active_buttons)
        return 0;
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
        return (int16_t)active_buttons[port];
    return id < 16 && (active_buttons[port] & masks[id]) ? 1 : 0;
}

static bool rumble_state(unsigned port, enum retro_rumble_effect effect, uint16_t strength)
{
    return psx_input_rumble_request(port, (unsigned)effect, strength) != 0;
}

