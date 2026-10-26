#ifndef TIZPSX_CORE_H
#define TIZPSX_CORE_H

#include <stddef.h>
#include <stdint.h>

/* Capacity for the renderer's 4x output; display scaling is independent. */
#define PSX_CORE_MAX_WIDTH 2048u
#define PSX_CORE_MAX_HEIGHT 1024u
#define PSX_CORE_AUDIO_RATE 44100u
#define PSX_CORE_AUDIO_MAX_FRAMES 2048u

enum psx_pad_button {
    PSX_PAD_B      = 1u << 0,
    PSX_PAD_Y      = 1u << 1,
    PSX_PAD_SELECT = 1u << 2,
    PSX_PAD_START  = 1u << 3,
    PSX_PAD_UP     = 1u << 4,
    PSX_PAD_DOWN   = 1u << 5,
    PSX_PAD_LEFT   = 1u << 6,
    PSX_PAD_RIGHT  = 1u << 7,
    PSX_PAD_A      = 1u << 8,
    PSX_PAD_X      = 1u << 9,
    PSX_PAD_L      = 1u << 10,
    PSX_PAD_R      = 1u << 11,
    PSX_PAD_L2     = 1u << 12,
    PSX_PAD_R2     = 1u << 13,
    PSX_PAD_L3     = 1u << 14,
    PSX_PAD_R3     = 1u << 15
};

/* Caller-owned video pixels are tightly packed RGB565, row-major. */
typedef struct {
    uint16_t pixels[PSX_CORE_MAX_WIDTH * PSX_CORE_MAX_HEIGHT];
    unsigned width;
    unsigned height;
    unsigned pitch_pixels;
    int valid;
} psx_video_frame_t;

typedef struct {
    /* Interleaved signed 16-bit stereo, 44100 Hz. */
    int16_t samples[PSX_CORE_AUDIO_MAX_FRAMES * 2u];
    size_t frames;
} psx_audio_frame_t;

/*
 * Initialize one user-supplied disc image and BIOS. Paths remain owned by the
 * caller. Return 0 on success. The core writes into caller-owned frame/audio
 * storage during psx_core_run(); it retains no output pointers after return.
 */
/* Internal resolution multiplier supported by the bundled GPU_NEON renderer:
 * 1 = native; 2..4 = enhanced 3D rendering. Other values fall back to 1. */
int psx_core_init(const char *bios_path, const char *content_path,
                  unsigned resolution_multiplier);
/* Validate a user BIOS image (readable recognized 512 KiB image). */
int psx_core_bios_supported(const char *path);
/* Analog axes are LX, LY, RX, RY, signed -32767..32767. */
/* Reuse video storage between calls without clearing it: libretro duplicate
 * frames preserve its previous pixels and metadata. Clear it for a new game. */
int psx_core_run(uint32_t p1_buttons, uint32_t p2_buttons,
                 const int16_t p1_analog[4], const int16_t p2_analog[4],
                 psx_video_frame_t *video, psx_audio_frame_t *audio);
void psx_core_set_analog_mode(int enabled);
unsigned psx_core_frame_rate_millihz(void);
void psx_core_deinit(void);
const char *psx_core_error(void);

#endif
