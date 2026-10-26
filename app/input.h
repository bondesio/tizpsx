#ifndef TIZPSX_INPUT_H
#define TIZPSX_INPUT_H

#include <stddef.h>
#include <stdint.h>
#include "psx_core.h"

enum {
    PSX_INPUT_CROSS, PSX_INPUT_CIRCLE, PSX_INPUT_SQUARE, PSX_INPUT_TRIANGLE,
    PSX_INPUT_START, PSX_INPUT_SELECT,
    PSX_INPUT_L1, PSX_INPUT_R1, PSX_INPUT_L2, PSX_INPUT_R2,
    PSX_INPUT_L3, PSX_INPUT_R3,
    PSX_INPUT_UP, PSX_INPUT_DOWN, PSX_INPUT_LEFT, PSX_INPUT_RIGHT,
    PSX_INPUT_BINDINGS
};

/* Retain the proven Neo Geo capture engine's generic binding vocabulary while
 * assigning its actions to the PlayStation controller layout. */
#define NG_INPUT_A PSX_INPUT_CROSS
#define NG_INPUT_B PSX_INPUT_CIRCLE
#define NG_INPUT_C PSX_INPUT_SQUARE
#define NG_INPUT_D PSX_INPUT_TRIANGLE
#define NG_INPUT_START PSX_INPUT_START
#define NG_INPUT_SELECT PSX_INPUT_SELECT
#define NG_INPUT_COIN PSX_INPUT_L1
#define NG_INPUT_L1 PSX_INPUT_L1
#define NG_INPUT_R1 PSX_INPUT_R1
#define NG_INPUT_L2 PSX_INPUT_L2
#define NG_INPUT_R2 PSX_INPUT_R2
#define NG_INPUT_L3 PSX_INPUT_L3
#define NG_INPUT_R3 PSX_INPUT_R3
#define NG_INPUT_UP PSX_INPUT_UP
#define NG_INPUT_DOWN PSX_INPUT_DOWN
#define NG_INPUT_LEFT PSX_INPUT_LEFT
#define NG_INPUT_RIGHT PSX_INPUT_RIGHT
#define NG_INPUT_BINDINGS PSX_INPUT_BINDINGS

#define NGP_A PSX_PAD_B
#define NGP_B PSX_PAD_A
#define NGP_C PSX_PAD_Y
#define NGP_D PSX_PAD_X
#define NGP_START PSX_PAD_START
#define NGP_SELECT PSX_PAD_SELECT
#define NGP_COIN PSX_PAD_L
#define NGP_UP PSX_PAD_UP
#define NGP_DOWN PSX_PAD_DOWN
#define NGP_LEFT PSX_PAD_LEFT
#define NGP_RIGHT PSX_PAD_RIGHT

/* Mapping steps cover all original DualShock buttons. Analog stick values are
 * also exposed to PCSX ReARMed when the selected controller provides axes. */
unsigned psx_input_mapping_binding(unsigned step);
uint16_t psx_input_binding_mask(unsigned binding);
void psx_input_init(const char *config_path);
void psx_input_shutdown(void);
void psx_input_pause_capture(void);
void psx_input_resume_capture(void);
void psx_input_poll(unsigned long long monotonic_ns);
int psx_input_recent_activity(unsigned long long monotonic_ns);
uint16_t psx_input_buttons(unsigned player);
int16_t psx_input_analog(unsigned player, unsigned axis);
int psx_input_rumble_enabled(void);
void psx_input_set_rumble_enabled(int enabled);
void psx_input_rumble_stop(void);
int psx_input_rumble_request(unsigned player, unsigned effect, uint16_t strength);
const char *psx_input_rumble_status(unsigned player);
const char *psx_input_status(unsigned player);
const char *psx_input_error(void);
int psx_input_changed(void);
void psx_input_cycle_device(unsigned player, int direction);
void psx_input_device_label(unsigned player, char *out, size_t capacity);
void psx_input_binding_label(unsigned player, unsigned binding, char *out, size_t capacity);
const char *psx_input_binding_name(unsigned binding);
void psx_input_reset(unsigned player);
void psx_input_unbind(unsigned player, unsigned binding);
void psx_input_capture(unsigned player, unsigned binding, unsigned long long now_ns);
void psx_input_cancel_capture(void);
int psx_input_capturing(void);
int psx_input_capture_succeeded(void);
int psx_input_exclusive(void);
void psx_input_toggle_exclusive(void);
const char *psx_input_capture_status(unsigned player);
void psx_input_test_begin(unsigned player);
void psx_input_test_cancel(void);
void psx_input_test_skip(void);
void psx_input_test_label(char *out, size_t capacity);

#endif
