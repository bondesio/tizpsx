/* Stable joydev discovery/mappings with optional, exactly matched evdev capture. */
#include "input.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <linux/input.h>
#ifdef PSX_INPUT_SELFTEST
/* Offline harness substitutes device/config I/O; it cannot open real nodes. */
#define open test_open
#define read test_read
#define write test_write
#define close test_close
#define fsync test_fsync
#define rename test_rename
#define ioctl test_ioctl
#define access test_access
#define realpath test_realpath
#endif
extern int open(const char *, int, ...);
extern long read(int, void *, unsigned long);
extern long write(int, const void *, unsigned long);
extern int close(int);
extern int fsync(int);
extern int rename(const char *, const char *);
extern int ioctl(int, unsigned long, ...);
extern int access(const char *, int);
extern char *realpath(const char *, char *);
extern void psx_diag(const char *);
extern int clock_gettime(int, struct timespec *);
#define CLOCK_MONOTONIC 1
#define DEVICES 32
#define BUTTONS 512
#define AXES 64
#define ID_SIZE 512
#define O_RDONLY 0
#define O_RDWR 2
#define O_NONBLOCK 0x800
#define O_CLOEXEC 0x80000
#define O_NOFOLLOW 0x8000 /* ARM Linux */
#define BTN_MISC 0x100
#define KEYS (BTN_MISC + BUTTONS)
/* Internal typed axis bindings cannot overlap Linux EV_KEY codes. */
#define AXIS_BINDING 1024
#define AXIS_BIND(code, positive) (AXIS_BINDING + (int)(code) * 2 + !!(positive))
#define EVIOCGKEY_STATE 0x80604518UL /* 768 bits */
#define MODELS 32
#define CONFIG_SIZE 65536 /* All 32 models with sixteen typed bindings. */
#define JSIOCGAXES 0x80016a11UL
#define JSIOCGBUTTONS 0x80016a12UL
#define JSIOCGAXMAP 0x80406a32UL
#define JSIOCGBTNMAP 0x84006a34UL
struct js_event { uint32_t time; int16_t value; uint8_t type, number; };
struct input_event32 { int32_t sec, usec; uint16_t type, code; int32_t value; };
struct absinfo { int32_t value, minimum, maximum, fuzz, flat, resolution; };
struct device {
    int fd, ev_fd, event_index, blocked, sync_lost, warned_key_query, warned_axis_query;
    int ev_writable, rumble_effect, rumble_available;
    uint16_t rumble_strong, rumble_weak;
    unsigned long long rumble_refresh_ns;
    char capture_status[112], capture_diag[160];
    char id[ID_SIZE], name[96];
    char sysfs_parent[4096];
    uint8_t axes, js_axes, buttons, axis_code[AXES];
    uint16_t button_code[BUTTONS];
    int16_t axis[AXES];
    uint8_t key[KEYS];
    int32_t minimum[AXES], maximum[AXES];
    uint8_t range_ok[AXES];
};
struct player { char id[ID_SIZE]; int binding[NG_INPUT_BINDINGS]; int device; };
static struct device devices[DEVICES];
static struct player players[2];
static struct { char id[80]; int binding[NG_INPUT_BINDINGS]; } models[MODELS];
static int exclusive_preferred = 1, model_limit, capture_paused;
static volatile int rumble_enabled = 1;
static volatile uint16_t rumble_request[2][2];
static int migrate_auto_slot[2];
static int test_player = -1, test_step, test_release, test_complete, test_wait_neutral;
static char test_detail[128];
static char capture_feedback_text[128];
static const unsigned mapping_order[NG_INPUT_BINDINGS] = {
    NG_INPUT_UP, NG_INPUT_DOWN, NG_INPUT_LEFT, NG_INPUT_RIGHT,
    NG_INPUT_C, NG_INPUT_D, NG_INPUT_A, NG_INPUT_B,
    NG_INPUT_L1, NG_INPUT_R1, NG_INPUT_L2, NG_INPUT_R2,
    NG_INPUT_L3, NG_INPUT_R3, NG_INPUT_SELECT, NG_INPUT_START
};
static char config_path[512];
static unsigned long long next_scan, capture_until, last_activity, poll_now, next_config_retry;
static int activity_valid, config_dirty;
static char config_error[160];
static unsigned scan_issue[DEVICES];
static int capture_player = -1, capture_binding, capture_armed, capture_success, changed;
static const uint16_t masks[NG_INPUT_BINDINGS] = {
    PSX_PAD_B, PSX_PAD_A, PSX_PAD_Y, PSX_PAD_X, PSX_PAD_START, PSX_PAD_SELECT,
    PSX_PAD_L, PSX_PAD_R, PSX_PAD_L2, PSX_PAD_R2, PSX_PAD_L3, PSX_PAD_R3,
    PSX_PAD_UP, PSX_PAD_DOWN, PSX_PAD_LEFT, PSX_PAD_RIGHT
};
static const unsigned defaults[NG_INPUT_BINDINGS] = {
    304, 305, 308, 307, 315, 314, 310, 311, 312, 313, 317, 318, 544, 545, 546, 547
};
static const unsigned classic[NG_INPUT_BINDINGS] = {
    288, 289, 290, 291, 297, 296, 292, 293, 294, 295, 298, 299, 544, 545, 546, 547
};
static const char *names[NG_INPUT_BINDINGS] = {
    "Cross", "Circle", "Square", "Triangle", "Start", "Select", "L1", "R1",
    "L2", "R2", "L3", "R3", "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right"
};

unsigned psx_input_mapping_binding(unsigned step)
{
    return step < NG_INPUT_BINDINGS ? mapping_order[step] : NG_INPUT_BINDINGS;
}
uint16_t psx_input_binding_mask(unsigned b) { return b < NG_INPUT_BINDINGS ? masks[b] : 0; }
static int directional(unsigned b) { return b >= NG_INPUT_UP && b < NG_INPUT_BINDINGS; }
static int axis_direction(int value) { return value < -12000 ? -1 : value > 12000 ? 1 : 0; }
static int dpad_key(unsigned code)
{
    return code == 103 || code == 108 || code == 105 || code == 106 ||
           (code >= 544 && code <= 547); /* BTN_DPAD_* on newer drivers. */
}
static int dpad_key_for_binding(unsigned binding, unsigned code)
{
    if (binding == NG_INPUT_UP) return code == 103 || code == 544;
    if (binding == NG_INPUT_DOWN) return code == 108 || code == 545;
    if (binding == NG_INPUT_LEFT) return code == 105 || code == 546;
    if (binding == NG_INPUT_RIGHT) return code == 106 || code == 547;
    return 0;
}
static int dpad_hat(unsigned code) { return code == 16 || code == 17; }
static int default_axis(unsigned b, unsigned code, int direction)
{
    if (b == NG_INPUT_LEFT) return code == 16 && direction < 0;
    if (b == NG_INPUT_RIGHT) return code == 16 && direction > 0;
    if (b == NG_INPUT_UP) return code == 17 && direction < 0;
    if (b == NG_INPUT_DOWN) return code == 17 && direction > 0;
    return 0;
}
static int axis_used(const struct device *d, unsigned a)
{
    unsigned code = d->axis_code[a];
    if (code == 0 || code == 1 || code == 16 || code == 17) return 1;
    for (unsigned p = 0; p < 2; ++p) for (unsigned b = NG_INPUT_UP; b < NG_INPUT_BINDINGS; ++b) {
        int v = players[p].binding[b];
        if (v >= AXIS_BINDING && (unsigned)(v - AXIS_BINDING) / 2 == code) return 1;
    }
    return 0;
}
static int read_text(const char *path, char *out, size_t capacity)
{
    int fd = open(path, O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { out[0] = 0; return 0; }
    long n = read(fd, out, capacity - 1); close(fd);
    if (n <= 0 || n == (long)capacity - 1) { out[0] = 0; return 0; }
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) --n;
    out[n] = 0;
    /* Config is line based; untrusted device strings must remain one line. */
    for (long i = 0; i < n; ++i)
        if ((unsigned char)out[i] < 32 || out[i] == '=') out[i] = '_';
    return n > 0;
}
static int identity(int index, char *id, char *name)
{
    char root[96], path[160], vendor[24], product[24], bus[24], uniq[160], phys[160];
    snprintf(root, sizeof root, "/sys/class/input/js%d/device", index);
    snprintf(path, sizeof path, "%s/id/vendor", root); if (!read_text(path, vendor, sizeof vendor)) return 0;
    snprintf(path, sizeof path, "%s/id/product", root); if (!read_text(path, product, sizeof product)) return 0;
    snprintf(path, sizeof path, "%s/id/bustype", root); if (!read_text(path, bus, sizeof bus)) return 0;
    snprintf(path, sizeof path, "%s/uniq", root); read_text(path, uniq, sizeof uniq);
    snprintf(path, sizeof path, "%s/phys", root); read_text(path, phys, sizeof phys);
    snprintf(path, sizeof path, "%s/name", root);
    if (!read_text(path, name, 96)) snprintf(name, 96, "Input %s:%s", vendor, product);
    if (uniq[0]) snprintf(id, ID_SIZE, "%s:%s:%s:serial:%s", bus, vendor, product, uniq);
    else if (phys[0]) snprintf(id, ID_SIZE, "%s:%s:%s:port:%s", bus, vendor, product, phys);
    else {
        char resolved[4096];
        if (!realpath(root, resolved)) return 0;
        /* Strip the kernel's volatile /input/inputN suffix, retaining the port. */
        char *suffix = strstr(resolved, "/input/input");
        if (!suffix) return 0;
        *suffix = 0;
        int prefix = snprintf(id, ID_SIZE, "%s:%s:%s:path:", bus, vendor, product);
        size_t length = strlen(resolved);
        if (prefix < 0 || (size_t)prefix + length >= ID_SIZE) return 0;
        memcpy(id + prefix, resolved, length + 1);
    }
    return 1;
}
static void write_rumble_event(struct device *d, int play)
{
    if (d->ev_fd < 0 || d->rumble_effect < 0 || !d->ev_writable) return;
    struct input_event32 event = {0};
    event.type = EV_FF;
    event.code = (uint16_t)d->rumble_effect;
    event.value = play;
    if (write(d->ev_fd, &event, sizeof event) != (long)sizeof event)
        d->rumble_available = 0;
}
static void apply_rumble(struct device *d, uint16_t strong, uint16_t weak)
{
    if (d->ev_fd < 0 || !d->ev_writable) return;
    if (!__atomic_load_n(&rumble_enabled, __ATOMIC_ACQUIRE)) strong = weak = 0;
    if (d->rumble_available == 0) return;
    if (strong == d->rumble_strong && weak == d->rumble_weak &&
        poll_now < d->rumble_refresh_ns + 30000000000ULL) return;
    struct ff_effect effect = {0};
    effect.type = FF_RUMBLE;
    effect.id = (int16_t)d->rumble_effect;
    effect.replay.length = 0xffff;
    effect.u.rumble.strong_magnitude = strong;
    effect.u.rumble.weak_magnitude = weak;
    if (ioctl(d->ev_fd, EVIOCSFF, &effect)) {
        d->rumble_available = 0;
        psx_diag("input force-feedback unsupported or unavailable");
        return;
    }
    d->rumble_effect = effect.id;
    d->rumble_available = 1;
    d->rumble_strong = strong;
    d->rumble_weak = weak;
    d->rumble_refresh_ns = poll_now;
    write_rumble_event(d, strong || weak);
}
static void release_capture(struct device *d)
{
    if (d->ev_fd >= 0) {
        if (d->rumble_effect >= 0) {
            write_rumble_event(d, 0);
            ioctl(d->ev_fd, EVIOCRMFF, d->rumble_effect);
        }
        ioctl(d->ev_fd, EVIOCGRAB, 0); /* Linux expects a value, not &zero. */
        close(d->ev_fd);
    }
    d->ev_fd = -1; d->event_index = -1; d->sync_lost = 0;
    d->ev_writable = 0; d->rumble_effect = -1; d->rumble_available = -1;
    d->rumble_strong = d->rumble_weak = 0; d->rumble_refresh_ns = 0;
    memset(d->key, 0, sizeof d->key); memset(d->axis, 0, sizeof d->axis);
    d->blocked = 1; changed = 1;
}
static void drop_device(int i)
{
    if (test_player >= 0 && players[test_player].device == i) { test_release = 1; test_wait_neutral = 1; }
    if (capture_player >= 0 && players[capture_player].device == i) capture_armed = 0;
    release_capture(&devices[i]);
    if (devices[i].fd >= 0) close(devices[i].fd);
    memset(&devices[i], 0, sizeof devices[i]); devices[i].fd = devices[i].ev_fd = -1; changed = 1;
}
static void save_config(void);
static int model_index(const char *id, int create)
{
    char model[80]; const char *end = id;
    for (int i = 0; i < 3; ++i) {
        end = strchr(end, ':');
        if (!end) return -1;
        if (i < 2) ++end;
    }
    size_t len = (size_t)(end - id);
    if (!len || len >= sizeof model) return -1;
    memcpy(model, id, len); model[len] = 0;
    int empty = -1;
    for (int i = 0; i < MODELS; ++i) {
        if (!strcmp(models[i].id, model)) return i;
        if (!models[i].id[0] && empty < 0) empty = i;
    }
    if (!create || empty < 0) { if (create && empty < 0) model_limit = 1; return -1; }
    snprintf(models[empty].id, sizeof models[empty].id, "%s", model);
    return empty;
}
static void sync_model(unsigned p)
{
    int m = model_index(players[p].id, 1);
    if (m < 0) return;
    memcpy(models[m].binding, players[p].binding, sizeof players[p].binding);
    for (unsigned other = 0; other < 2; ++other)
        if (other != p && model_index(players[other].id, 0) == m)
            memcpy(players[other].binding, models[m].binding, sizeof players[other].binding);
}
static int migrate_dpad_binding(int binding)
{
    if (binding >= AXIS_BINDING) {
        unsigned code = (unsigned)(binding - AXIS_BINDING) / 2;
        return dpad_hat(code) ? binding : 0;
    }
    if (binding > 0 && !dpad_key((unsigned)binding)) return 0;
    return binding;
}
static int name_has(const char *name, const char *word);
static int virtual_device(const struct device *d)
{
    return strstr(d->id, "path:/sys/devices/virtual") != NULL ||
           name_has(d->name, "virtual gamepad") || name_has(d->name, "tizen virtual");
}
static void assign_devices(void)
{
    int remember = 0;
    int used[DEVICES] = {0};
    int old[2] = {players[0].device, players[1].device};
    /* A physical pad cannot drive both players. Keep P1's explicit choice and
     * let P2 auto-select the next available device (normally Tizen Virtual). */
    if (players[1].id[0] && strcmp(players[1].id, "none") &&
        !strcmp(players[1].id, players[0].id)) {
        players[1].id[0] = 0;
        remember = 1;
        psx_diag("input duplicate device assignment; Player 2 returned to Auto");
    }
    for (unsigned p = 0; p < 2; ++p) {
        players[p].device = -1;
        if (!players[p].id[0] || !strcmp(players[p].id, "none")) continue;
        for (int d = 0; d < DEVICES; ++d)
            if (!used[d] && devices[d].fd >= 0 && !strcmp(players[p].id, devices[d].id)) {
                players[p].device = d; used[d] = 1; break;
            }
    }
    for (unsigned p = 0; p < 2; ++p) {
        if (players[p].id[0]) continue;
        /* Retain an automatic assignment when a lower js index reconnects. */
        int d = old[p];
        if (d >= 0 && !used[d] && devices[d].fd >= 0) { players[p].device = d; used[d] = 1; }
        else for (int pass = 0; pass < 2 && players[p].device < 0; ++pass)
            for (d = 0; d < DEVICES; ++d)
                if (!used[d] && devices[d].fd >= 0 &&
                    (pass || !virtual_device(&devices[d]))) {
                    players[p].device = d; used[d] = 1; break;
                }
    }
    for (unsigned p = 0; p < 2; ++p) {
        int d = players[p].device;
        if (!players[p].id[0] && d >= 0 && devices[d].id[0]) {
            snprintf(players[p].id, ID_SIZE, "%s", devices[d].id); remember = 1;
            /* Legacy Auto slots had no hardware ID yet. Preserve their saved
             * bindings exactly once, unless that model already has a map. */
            if (migrate_auto_slot[p] && model_index(players[p].id, 0) < 0) sync_model(p);
            migrate_auto_slot[p] = 0;
        }
    }
    for (unsigned p = 0; p < 2; ++p) if (old[p] != players[p].device) {
        if (players[p].device >= 0) {
            struct device *d = &devices[players[p].device];
            d->blocked = 1;
            int m = model_index(d->id, 1);
            if (m >= 0) memcpy(players[p].binding, models[m].binding, sizeof players[p].binding);
            else if (d->id[0]) memset(players[p].binding, 0, sizeof players[p].binding);
        }
        changed = 1;
    }
    if (remember) save_config();
}
static int has_hex_bits(const char *text)
{
    for (; *text; ++text)
        if ((*text >= '1' && *text <= '9') || (*text >= 'a' && *text <= 'f') || (*text >= 'A' && *text <= 'F')) return 1;
    return 0;
}
static int name_has(const char *name, const char *word)
{
    for (; *name; ++name) {
        const char *n = name, *w = word;
        while (*n && *w) {
            char c = *n >= 'A' && *n <= 'Z' ? *n + ('a' - 'A') : *n;
            if (c != *w) break;
            ++n; ++w;
        }
        if (!*w) return 1;
    }
    return 0;
}
static int controller_capabilities(int index, const struct device *d, const char *name)
{
    char path[128], keys[512], properties[128];
    snprintf(path, sizeof path, "/sys/class/input/js%d/device/capabilities/key", index);
    int has_keys = read_text(path, keys, sizeof keys) && has_hex_bits(keys);
    snprintf(path, sizeof path, "/sys/class/input/js%d/device/properties", index);
    int has_properties = read_text(path, properties, sizeof properties);
    /* INPUT_PROP_ACCELEROMETER is bit 6 in the final hex word. */
    unsigned low_properties = 0;
    if (has_properties) for (const char *c = properties; *c; ++c) {
        if (*c == ' ') low_properties = 0;
        else if (*c >= '0' && *c <= '9') low_properties = (low_properties << 4) | (*c - '0');
        else if (*c >= 'a' && *c <= 'f') low_properties = (low_properties << 4) | (*c - 'a' + 10);
        else if (*c >= 'A' && *c <= 'F') low_properties = (low_properties << 4) | (*c - 'A' + 10);
        else return 0;
    }
    if ((low_properties & 64) || name_has(name, "motion") || name_has(name, "sensor") ||
        name_has(name, "accelerometer") || name_has(name, "gyroscope")) return 0;
    /* A normal pad must have real key capabilities and at least one Linux
     * button, not just a joydev node generated from motion axes. */
    if (has_keys && d->buttons) {
        for (unsigned b = 0; b < d->buttons; ++b)
            if (d->button_code[b] >= BTN_MISC && d->button_code[b] < BTN_MISC + BUTTONS) return 1;
    }
    /* Keep identifiable analog-only controls, but require readable properties,
     * a known control class and an axis that this frontend can actually use. */
    if (!d->buttons && has_properties &&
        (name_has(name, "joystick") || name_has(name, "gamepad") || name_has(name, "wheel") ||
         name_has(name, "flightstick") || name_has(name, "yoke") || name_has(name, "pedal"))) {
        for (unsigned a = 0; a < d->axes; ++a)
            if (d->axis_code[a] == 0 || d->axis_code[a] == 1 || d->axis_code[a] == 16 || d->axis_code[a] == 17) return 1;
    }
    return 0;
}
static int joydev_button_capable(const struct device *d)
{
    for (unsigned b = 0; b < d->buttons; ++b)
        if (d->button_code[b] >= BTN_MISC && d->button_code[b] < BTN_MISC + BUTTONS) return 1;
    return 0;
}
static void fallback_joydev_axis_map(struct device *d)
{
    for (unsigned a = 0; a < d->axes; ++a) d->axis_code[a] = 0xff;
    if (d->axes > 0) d->axis_code[0] = 0;   /* ABS_X */
    if (d->axes > 1) d->axis_code[1] = 1;   /* ABS_Y */
    if (d->axes >= 4) {
        d->axis_code[2] = 3;               /* ABS_RX */
        d->axis_code[3] = 4;               /* ABS_RY */
    }
    if (d->axes >= 8) {
        d->axis_code[6] = 16;              /* ABS_HAT0X */
        d->axis_code[7] = 17;              /* ABS_HAT0Y */
    }
}
static void fallback_joydev_button_map(struct device *d)
{
    /* Same conventional BTN_GAMEPAD ordering used by TizNES/TizGB when
     * JSIOCGBTNMAP is unavailable, extended through Neo Geo's C/D/Coin keys. */
    static const uint16_t standard[15] = {
        304, 305, 306, 307, 308, 309, 310, 311,
        314, 315, 312, 313, 316, 317, 318
    };
    unsigned count = d->buttons < 15 ? d->buttons : 15;
    for (unsigned b = 0; b < count; ++b) d->button_code[b] = standard[b];
}
static int sensor_name(const char *name)
{
    return name_has(name, "motion") || name_has(name, "sensor") ||
           name_has(name, "accelerometer") || name_has(name, "gyroscope");
}
static int accelerometer_property(int index)
{
    char path[128], properties[128];
    snprintf(path, sizeof path, "/sys/class/input/js%d/device/properties", index);
    if (!read_text(path, properties, sizeof properties)) return 0;
    unsigned low = 0;
    for (const char *c = properties; *c; ++c) {
        if (*c == ' ') low = 0;
        else if (*c >= '0' && *c <= '9') low = (low << 4) | (unsigned)(*c - '0');
        else if (*c >= 'a' && *c <= 'f') low = (low << 4) | (unsigned)(*c - 'a' + 10);
        else if (*c >= 'A' && *c <= 'F') low = (low << 4) | (unsigned)(*c - 'A' + 10);
        else return 0;
    }
    return (low & 64) != 0;
}
static void scan_devices(void)
{
    for (int i = 0; i < DEVICES; ++i) {
        char id[ID_SIZE], name[96], path[64], parent[4096];
        snprintf(path, sizeof path, "/sys/class/input/js%d/device", i);
        int have_parent = realpath(path, parent) != 0;
        int have_identity = identity(i, id, name);
        if (!have_identity) {
            char name_path[128]; id[0] = 0; name[0] = 0;
            snprintf(name_path, sizeof name_path, "/sys/class/input/js%d/device/name", i);
            read_text(name_path, name, sizeof name);
        }
        /* A readable joydev pad is still usable when app-context sysfs access
         * cannot provide its stable identity. Auto assignment remains
         * transient; update_capture separately pairs this js node to evdev. */
        int same_identity = have_identity && devices[i].id[0] && !strcmp(id, devices[i].id);
        int same_parent = have_parent && devices[i].sysfs_parent[0] &&
                          !strcmp(parent, devices[i].sysfs_parent);
        if (devices[i].fd >= 0 &&
            ((!have_identity && !have_parent) ||
             (have_parent && same_parent && (!have_identity || same_identity)) ||
             (!have_parent && have_identity && same_identity))) continue;
        if (devices[i].fd >= 0) drop_device(i);
        snprintf(path, sizeof path, "/dev/input/js%d", i);
        /* Match the proven NES/GB joydev open flags on this TV. */
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            int error = *__errno_location();
            if (error != 2 && !scan_issue[i]) {
                char diag[128]; snprintf(diag, sizeof diag, "input js%d open_errno=%d", i, error);
                psx_diag(diag); scan_issue[i] = 1;
            }
            continue;
        }
        char parent_after[4096];
        snprintf(path, sizeof path, "/sys/class/input/js%d/device", i);
        int parent_after_ok = realpath(path, parent_after) != 0;
        if (have_parent && (!parent_after_ok || strcmp(parent, parent_after))) {
            close(fd);
            if (!scan_issue[i]) { psx_diag("input joydev topology changed during open"); scan_issue[i] = 1; }
            continue;
        }
        struct device *d = &devices[i];
        memset(d, 0, sizeof *d); d->fd = fd; d->ev_fd = -1; d->event_index = -1; d->blocked = 1;
        int ioctl_failed = ioctl(fd, JSIOCGAXES, &d->axes) || ioctl(fd, JSIOCGBUTTONS, &d->buttons) || d->axes > AXES;
        int axis_map_ok = !ioctl_failed && ioctl(fd, JSIOCGAXMAP, d->axis_code) == 0;
        int button_map_ok = !ioctl_failed && ioctl(fd, JSIOCGBTNMAP, d->button_code) == 0;
        if (!ioctl_failed && !axis_map_ok) fallback_joydev_axis_map(d);
        if (!ioctl_failed && !button_map_ok) fallback_joydev_button_map(d);
        d->js_axes = d->axes;
        int sysfs_capable = !ioctl_failed && controller_capabilities(i, d, name);
        int explicitly_sensor = sensor_name(name) || accelerometer_property(i);
        /* Joydev supplies a valid Linux button map even on firmware/app
         * contexts where the sysfs capability bitmap cannot be read. Accept
         * only real button codes and recognizable non-sensor names there. */
        int ioctl_capable = !ioctl_failed && joydev_button_capable(d) && !explicitly_sensor;
        if (ioctl_failed || (!sysfs_capable && !ioctl_capable)) {
            /* A rejected candidate was never exposed to a player. Avoid a
             * false topology change on every scan of a sensor-only node. */
            int error = *__errno_location();
            if (!scan_issue[i]) {
                char diag[160];
                snprintf(diag, sizeof diag, "input js%d rejected stage=%s errno=%d axes=%u buttons=%u",
                         i, ioctl_failed ? "joydev-count-ioctl" : "capabilities", error, d->axes, d->buttons);
                psx_diag(diag); scan_issue[i] = 1;
            }
            close(fd); memset(d, 0, sizeof *d); d->fd = d->ev_fd = -1; continue;
        }
        snprintf(d->id, sizeof d->id, "%s", id);
        if (parent_after_ok) snprintf(d->sysfs_parent, sizeof d->sysfs_parent, "%s", parent_after);
        snprintf(d->name, sizeof d->name, "%s", name[0] ? name : "Gamepad");
        if (!have_identity && !scan_issue[i]) {
            char diag[128]; snprintf(diag, sizeof diag, "input js%d stable-identity-unavailable; using Auto slot", i);
            psx_diag(diag);
        }
        scan_issue[i] = 0; changed = 1;
    }
    assign_devices();
}
static void config_failure(const char *message)
{
    if (strcmp(config_error, message)) {
        snprintf(config_error, sizeof config_error, "%s", message);
        psx_diag(message); changed = 1;
    }
    next_config_retry = poll_now + 1000000000ULL;
}
/* Accept the original numeric records and explicit typed records. Axis IDs
 * are Linux ABS codes, never the packed joydev index (TizNES/TizGB contract). */
static int parse_uint(const char **text, unsigned limit, unsigned *out)
{
    const char *v = *text; unsigned n = 0;
    if (*v < '0' || *v > '9') return 0;
    do {
        unsigned digit = (unsigned)(*v++ - '0');
        if (digit > limit || n > (limit - digit) / 10) return 0;
        n = n * 10 + digit;
    } while (*v >= '0' && *v <= '9');
    *out = n; *text = v; return 1;
}
static int parse_binding(const char *text, unsigned b, int *out)
{
    unsigned value;
    if (!strcmp(text, "-1")) { *out = -1; return 1; }
    if (!strcmp(text, "0")) { *out = 0; return 1; }
    if (!strncmp(text, "axis:", 5)) {
        text += 5;
        if (!directional(b) || !parse_uint(&text, AXES - 1, &value) || *text++ != ':') return 0;
        if (strcmp(text, "-1") && strcmp(text, "+1") && strcmp(text, "1")) return 0;
        *out = AXIS_BIND(value, *text != '-'); return 1;
    }
    if (!strncmp(text, "key:", 4)) text += 4;
    if (!parse_uint(&text, KEYS - 1, &value) || *text || !value) return 0;
    *out = (int)value; return 1;
}
static int parse_record(const char *text, unsigned *b, int *value)
{
    return parse_uint(&text, NG_INPUT_BINDINGS - 1, b) && *text++ == '=' && parse_binding(text, *b, value);
}
static void binding_record(int value, char *out, size_t capacity)
{
    if (value >= AXIS_BINDING)
        snprintf(out, capacity, "axis:%d:%s1", (value - AXIS_BINDING) / 2, (value - AXIS_BINDING) & 1 ? "+" : "-");
    else if (value > 0) snprintf(out, capacity, "key:%d", value);
    else snprintf(out, capacity, "%d", value);
}
static void persist_config(void)
{
    char data[CONFIG_SIZE], tmp[544], directory[512];
    size_t used = (size_t)snprintf(data, sizeof data, "exclusive=%d\nrumble=%d\n",
        exclusive_preferred, __atomic_load_n(&rumble_enabled, __ATOMIC_ACQUIRE));
    for (unsigned p = 0; p < 2; ++p) {
        int n = snprintf(data + used, sizeof data - used, "p%u.device=%s\n", p + 1, players[p].id[0] ? players[p].id : "auto");
        if (n < 0 || (size_t)n >= sizeof data - used) { config_failure("Controller settings too large; changes pending"); return; }
        used += n;
        for (unsigned b = 0; b < NG_INPUT_BINDINGS; ++b) {
            char record[32]; binding_record(players[p].binding[b], record, sizeof record);
            n = snprintf(data + used, sizeof data - used, "p%u.%u=%s\n", p + 1, b, record);
            if (n < 0 || (size_t)n >= sizeof data - used) { config_failure("Controller settings too large; changes pending"); return; }
            used += n;
        }
    }
    for (int m = 0; m < MODELS; ++m) if (models[m].id[0]) {
        for (unsigned b = 0; b < NG_INPUT_BINDINGS; ++b) {
            char record[32]; binding_record(models[m].binding[b], record, sizeof record);
            int n = snprintf(data + used, sizeof data - used, "m.%s.%u=%s\n", models[m].id, b, record);
            if (n < 0 || (size_t)n >= sizeof data - used) { config_failure("Controller settings too large; changes pending"); return; }
            used += n;
        }
    }
    snprintf(tmp, sizeof tmp, "%s.tmp", config_path);
    snprintf(directory, sizeof directory, "%s", config_path);
    char *slash = strrchr(directory, '/');
    if (slash == directory) slash[1] = 0;
    else if (slash) *slash = 0;
    else snprintf(directory, sizeof directory, ".");
    int dir_fd = open(directory, 0x4000 | O_CLOEXEC); /* ARM O_DIRECTORY */
    if (dir_fd < 0) { config_failure("Cannot open controller settings directory; retrying"); return; }
    int fd = open(tmp, 1 | 0x40 | 0x200 | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) { close(dir_fd); config_failure("Cannot open controller settings file; retrying"); return; }
    const char *failure = 0;
    size_t done = 0;
    while (done < used) {
        long n = write(fd, data + done, used - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { failure = "Cannot write controller settings; retrying"; break; }
        done += n;
    }
    if (!failure && fsync(fd)) failure = "Cannot sync controller settings; retrying";
    if (close(fd) && !failure) failure = "Cannot close controller settings; retrying";
    if (!failure && rename(tmp, config_path)) failure = "Cannot replace controller settings; retrying";
    if (!failure && fsync(dir_fd)) failure = "Cannot sync controller settings directory; retrying";
    if (close(dir_fd) && !failure) failure = "Cannot close controller settings directory; retrying";
    if (failure) { config_failure(failure); return; }
    config_dirty = 0;
    if (config_error[0]) { config_error[0] = 0; changed = 1; psx_diag("Controller settings saved after retry"); }
}
static void save_config(void)
{
    config_dirty = 1;
    persist_config();
}
const char *psx_input_error(void) { return config_error[0] ? config_error : model_limit ? "Controller model limit reached (32); new models use slot-only mappings" : ""; }
const char *psx_input_status(unsigned player)
{
    if (player > 1 || players[player].device < 0) return "Not connected";
    return devices[players[player].device].name;
}
int psx_input_rumble_enabled(void)
{
    return __atomic_load_n(&rumble_enabled, __ATOMIC_ACQUIRE);
}
void psx_input_set_rumble_enabled(int enabled)
{
    enabled = !!enabled;
    if (enabled == psx_input_rumble_enabled()) return;
    __atomic_store_n(&rumble_enabled, enabled, __ATOMIC_RELEASE);
    if (!enabled) for (unsigned p = 0; p < 2; ++p) {
        __atomic_store_n(&rumble_request[p][0], 0, __ATOMIC_RELEASE);
        __atomic_store_n(&rumble_request[p][1], 0, __ATOMIC_RELEASE);
    }
    save_config(); changed = 1;
}
void psx_input_rumble_stop(void)
{
    for (unsigned p = 0; p < 2; ++p)
        for (unsigned effect = 0; effect < 2; ++effect)
            __atomic_store_n(&rumble_request[p][effect], 0, __ATOMIC_RELEASE);
}
int psx_input_rumble_request(unsigned player, unsigned effect, uint16_t strength)
{
    if (player > 1 || effect > 1 || !psx_input_rumble_enabled()) return 0;
    __atomic_store_n(&rumble_request[player][effect], strength, __ATOMIC_RELEASE);
    return 1;
}
const char *psx_input_rumble_status(unsigned player)
{
    if (!psx_input_rumble_enabled()) return "Disabled";
    if (player > 1 || players[player].device < 0) return "No selected controller";
    struct device *d = &devices[players[player].device];
    if (d->ev_fd < 0) return "Needs exclusive event access";
    if (!d->ev_writable) return "Controller event device is read-only";
    if (d->rumble_available == 0) return "Controller force feedback unavailable";
    if (d->rumble_available == 1) return "Ready";
    return "Enabled; checking when a game requests rumble";
}

void psx_input_init(const char *path)
{
    __atomic_store_n(&rumble_request[0][0], 0, __ATOMIC_RELAXED);
    __atomic_store_n(&rumble_request[0][1], 0, __ATOMIC_RELAXED);
    __atomic_store_n(&rumble_request[1][0], 0, __ATOMIC_RELAXED);
    __atomic_store_n(&rumble_request[1][1], 0, __ATOMIC_RELAXED);
    for (int i = 0; i < DEVICES; ++i) devices[i].fd = devices[i].ev_fd = -1;
    for (unsigned p = 0; p < 2; ++p) { players[p].device = -1; migrate_auto_slot[p] = 0; }
    snprintf(config_path, sizeof config_path, "%s", path);
    char data[CONFIG_SIZE]; int fd = open(path, O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd >= 0) {
        long n = read(fd, data, sizeof data - 1); close(fd);
        if (n > 0 && n < (long)sizeof data - 1) {
            data[n] = 0;
            char *line = data;
            while (*line) {
                char *end = line; while (*end && *end != '\n') ++end;
                char saved = *end; *end = 0;
                if (!strcmp(line, "exclusive=0")) exclusive_preferred = 0;
                else if (!strcmp(line, "exclusive=1")) exclusive_preferred = 1;
                else if (!strcmp(line, "rumble=0")) rumble_enabled = 0;
                else if (!strcmp(line, "rumble=1")) rumble_enabled = 1;
                else if (!strncmp(line, "m.", 2)) {
                    char *dot = strrchr(line + 2, '.');
                    unsigned b; int value;
                    if (dot && parse_record(dot + 1, &b, &value)) {
                        *dot = 0;
                        char full[96];
                        int length = snprintf(full, sizeof full, "%s:stored", line + 2);
                        if (length > 0 && (size_t)length < sizeof full) {
                            int m = model_index(full, 1);
                            if (m >= 0) models[m].binding[b] = value;
                        }
                    }
                }
                else if (line[0] == 'p' && (line[1] == '1' || line[1] == '2') && line[2] == '.') {
                    unsigned p = line[1] - '1';
                    if (!strncmp(line + 3, "device=", 7) && strlen(line + 10) < ID_SIZE) {
                        snprintf(players[p].id, ID_SIZE, "%s", !strcmp(line + 10, "auto") ? "" : line + 10);
                    } else {
                        unsigned b; int value;
                        if (parse_record(line + 3, &b, &value)) {
                            players[p].binding[b] = value; migrate_auto_slot[p] = 1;
                        }
                    }
                }
                if (!saved) break;
                line = end + 1;
            }
        }
    }
    /* One-time migration: preserve existing slot bindings for the saved model.
     * New models start with defaults; switching pads cannot overwrite this map. */
    for (unsigned p = 0; p < 2; ++p) {
        for (unsigned b = NG_INPUT_UP; b < NG_INPUT_BINDINGS; ++b)
            players[p].binding[b] = migrate_dpad_binding(players[p].binding[b]);
        if (model_index(players[p].id, 0) < 0) sync_model(p);
        if (players[p].id[0]) migrate_auto_slot[p] = 0;
    }
    for (int m = 0; m < MODELS; ++m) if (models[m].id[0])
        for (unsigned b = NG_INPUT_UP; b < NG_INPUT_BINDINGS; ++b)
            models[m].binding[b] = migrate_dpad_binding(models[m].binding[b]);
    next_scan = 0; changed = 1; save_config();
}
static int neutral(const struct device *d, int include_axes)
{
    for (unsigned code = 0; code < KEYS; ++code) if (d->key[code]) return 0;
    if (include_axes) for (unsigned a = 0; a < d->axes; ++a)
        if (axis_used(d, a) && axis_direction(d->axis[a])) return 0;
    return 1;
}
static void capture_note(struct device *d, const char *status, const char *diagnostic)
{
    if (strcmp(d->capture_status, status)) {
        snprintf(d->capture_status, sizeof d->capture_status, "%s", status); changed = 1;
    }
    if (strcmp(d->capture_diag, diagnostic)) {
        snprintf(d->capture_diag, sizeof d->capture_diag, "%s", diagnostic); psx_diag(diagnostic);
    }
}
static void capture_feedback(const char *message)
{
    if (strcmp(capture_feedback_text, message)) {
        snprintf(capture_feedback_text, sizeof capture_feedback_text, "%s", message);
        char line[180];
        snprintf(line, sizeof line, "input-map: %s", message);
        psx_diag(line); changed = 1;
    }
}
static int selected_device(int i)
{
    return players[0].device == i || players[1].device == i;
}
static int normalized_axis(const struct device *d, unsigned a, int32_t value)
{
    if (!d->range_ok[a]) return 0;
    int64_t span = (int64_t)d->maximum[a] - d->minimum[a];
    int64_t v = ((int64_t)value - d->minimum[a]) * 65534 / span - 32767;
    if (v < -32767) v = -32767;
    if (v > 32767) v = 32767;
    return (int)v;
}
static int register_evdev_hat_axis(int index, struct device *d, unsigned code)
{
    if (code != ABS_HAT0X && code != ABS_HAT0Y) return -1;
    for (unsigned a = 0; a < d->axes; ++a) if (d->axis_code[a] == code) return (int)a;
    if (d->axes >= AXES) return -1;
    unsigned a = d->axes++;
    d->axis_code[a] = (uint8_t)code;
    struct absinfo info;
    if (ioctl(d->ev_fd, EVIOCGABS(code), &info) == 0 && info.maximum > info.minimum) {
        d->minimum[a] = info.minimum; d->maximum[a] = info.maximum;
        d->range_ok[a] = 1;
        d->axis[a] = normalized_axis(d, a, info.value);
    } else {
        /* Linux hat switches report -1, 0, +1. */
        d->minimum[a] = -1; d->maximum[a] = 1;
        d->range_ok[a] = 1; d->axis[a] = 0;
    }
    char line[128];
    snprintf(line, sizeof line, "input js%d registered evdev HAT axis code=%u slot=%u (joydev omitted it)",
             index, code, a);
    psx_diag(line);
    return (int)a;
}
static int resync_evdev(int index, struct device *d)
{
    uint8_t keys[96] = {0};
    int key_query_failed = ioctl(d->ev_fd, EVIOCGKEY_STATE, keys) != 0;
    int key_error = key_query_failed ? errno : 0;
    if (key_query_failed) {
        /* Some Tizen joydev/evdev drivers reject state snapshots even though
         * they stream events and accept EVIOCGRAB. Start neutral and rely on
         * subsequent edges; never throw away a successful exclusive grab. */
        memset(d->key, 0, sizeof d->key);
        if (!d->warned_key_query) {
            char line[128];
            snprintf(line, sizeof line, "input js%d EVIOCGKEY unavailable errno=%d; starting neutral", index, key_error);
            psx_diag(line); d->warned_key_query = 1;
        }
    } else {
        memset(d->key, 0, sizeof d->key);
        for (unsigned code = 0; code < KEYS; ++code)
            d->key[code] = !!(keys[code >> 3] & (1u << (code & 7)));
    }
    for (unsigned a = 0; a < d->axes; ++a) {
        unsigned code = d->axis_code[a];
        if (code >= AXES) continue;
        struct absinfo info;
        int axis_query_failed = ioctl(d->ev_fd, EVIOCGABS(code), &info) != 0;
        int axis_error = axis_query_failed ? errno : 0;
        if (axis_query_failed || info.maximum <= info.minimum) {
            if (axis_used(d, a)) {
                /* Stick axes use a signed range; hat switches report -1..1.
                 * Normalize both so directional thresholds remain meaningful. */
                int hat = code >= 16 && code <= 23;
                d->minimum[a] = hat ? -1 : -32767;
                d->maximum[a] = hat ? 1 : 32767;
                d->range_ok[a] = 1; d->axis[a] = 0;
                if (!d->warned_axis_query) {
                    char line[144];
                    snprintf(line, sizeof line, "input js%d EVIOCGABS(%u) unavailable errno=%d; using %s range",
                             index, code, axis_error, hat ? "hat" : "signed");
                    psx_diag(line); d->warned_axis_query = 1;
                }
            } else d->range_ok[a] = 0;
            continue;
        }
        d->minimum[a] = info.minimum; d->maximum[a] = info.maximum; d->range_ok[a] = 1;
        d->axis[a] = normalized_axis(d, a, info.value);
    }
    return 1;
}
static void update_capture(void)
{
    for (int i = 0; i < DEVICES; ++i) {
        struct device *d = &devices[i];
        if (d->fd < 0) continue;
        if (capture_paused) {
            if (d->ev_fd >= 0) release_capture(d);
            capture_note(d, "paused; controller capture released", "input evdev=released-on-pause");
            continue;
        }
        if (!exclusive_preferred || !selected_device(i)) {
            if (d->ev_fd >= 0) release_capture(d);
            capture_note(d, "shared; Tizen shortcuts may still fire", "input evdev=shared");
            continue;
        }
        char path[96], js_parent[4096], event_parent[4096], diag[160], status[112];
        snprintf(path, sizeof path, "/sys/class/input/js%d/device", i);
        if (!d->sysfs_parent[0] || !realpath(path, js_parent) ||
            strcmp(js_parent, d->sysfs_parent)) {
            if (d->ev_fd >= 0) release_capture(d);
            snprintf(diag, sizeof diag, "input js%d evdev=waiting-for-current-sysfs-parent", i);
            capture_note(d, "waiting for device access; shared fallback (sysfs unavailable)", diag);
            continue;
        }
        if (d->ev_fd >= 0) continue;
        int matches = 0, event = -1;
        /* Once a unique matching event node has been found, remember it even
         * when opening is temporarily denied. Re-resolve and verify its
         * exact sysfs parent on each retry, but do not walk all 256 event
         * paths every second for the same inaccessible device. */
        if (d->event_index >= 0) {
            snprintf(path, sizeof path, "/sys/class/input/event%d/device", d->event_index);
            if (realpath(path, event_parent) && !strcmp(js_parent, event_parent)) {
                event = d->event_index; matches = 1;
            } else d->event_index = -1;
        }
        if (event < 0 && d->buttons) {
            /* Never open candidates until exact sysfs-parent equality succeeds. */
            for (int e = 0; e < 256; ++e) {
                snprintf(path, sizeof path, "/sys/class/input/event%d/device", e);
                if (realpath(path, event_parent) && !strcmp(js_parent, event_parent)) { ++matches; event = e; }
            }
        }
        if (matches != 1) {
            snprintf(diag, sizeof diag, "input js%d evdev=no-unique-sysfs-match count=%d", i, matches);
            capture_note(d, "waiting for device access; shared fallback (no unique event)", diag); continue;
        }
        d->event_index = event;
        snprintf(path, sizeof path, "/dev/input/event%d", event);
        int can_r = access(path, 4) == 0, can_w = access(path, 2) == 0;
        int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
        int ev_writable = fd >= 0;
        if (fd < 0) fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) {
            int error = errno;
            snprintf(diag, sizeof diag, "input js%d event%d open_errno=%d access_r=%d access_w=%d", i, event, error, can_r, can_w);
            snprintf(status, sizeof status, "waiting for device access; shared fallback (event%d errno %d)", event, error);
            capture_note(d, status, diag); continue;
        }
        /* Recheck topology after opening; re-enumeration must never grab a
         * different device just because an event number was recycled. */
        char js_parent_after[4096];
        snprintf(path, sizeof path, "/sys/class/input/js%d/device", i);
        int js_parent_valid = realpath(path, js_parent_after) != 0;
        snprintf(path, sizeof path, "/sys/class/input/event%d/device", event);
        char id[ID_SIZE], name[96];
        int have_identity = identity(i, id, name);
        if (!js_parent_valid || strcmp(js_parent, js_parent_after) ||
            (d->sysfs_parent[0] && strcmp(js_parent_after, d->sysfs_parent)) ||
            !realpath(path, event_parent) || strcmp(js_parent_after, event_parent) ||
            (have_identity && d->id[0] && strcmp(id, d->id))) {
            close(fd);
            snprintf(diag, sizeof diag, "input js%d event%d topology-changed-after-open", i, event);
            capture_note(d, "waiting for device access; shared fallback (topology changed)", diag);
            continue;
        }
        if (ioctl(fd, EVIOCGRAB, 1)) {
            int error = errno; close(fd);
            snprintf(diag, sizeof diag, "input js%d event%d grab_errno=%d", i, event, error);
            snprintf(status, sizeof status, "waiting for device access; shared fallback (grab errno %d)", error);
            capture_note(d, status, diag); continue;
        }
        d->ev_fd = fd; d->ev_writable = ev_writable; d->rumble_effect = -1;
        d->rumble_available = ev_writable ? -1 : 0;
        if (ev_writable) {
            unsigned long ff_bits[(FF_MAX + sizeof(unsigned long) * 8) /
                                  (sizeof(unsigned long) * 8)] = {0};
            if (!ioctl(fd, EVIOCGBIT(EV_FF, sizeof ff_bits), ff_bits) &&
                !(ff_bits[FF_RUMBLE / (sizeof(unsigned long) * 8)] &
                  (1UL << (FF_RUMBLE % (sizeof(unsigned long) * 8)))))
                d->rumble_available = 0;
        }
        d->event_index = event; d->blocked = 1; activity_valid = 0;
        if (capture_player >= 0 && players[capture_player].device == i) capture_armed = 0;
        memset(d->axis, 0, sizeof d->axis);
        resync_evdev(i, d);
        snprintf(diag, sizeof diag, "input js%d evdev=exclusive event=%d", i, event);
        capture_note(d, "exclusive capture active", diag);
    }
}
static unsigned binding_code(unsigned p, unsigned b, const struct device *d)
{
    int code = players[p].binding[b];
    if (code < 0 || code >= AXIS_BINDING) return 0;
    if (code) return (unsigned)code;
    for (unsigned i = 0; i < d->buttons; ++i) if (d->button_code[i] == defaults[b]) return defaults[b];
    for (unsigned i = 0; i < d->buttons; ++i) if (d->button_code[i] == classic[b]) return classic[b];
    return defaults[b];
}
static int source_claimed(unsigned p, unsigned b, int source)
{
    for (unsigned other = 0; other < NG_INPUT_BINDINGS; ++other)
        if (other != b && players[p].binding[other] == source) return 1;
    return 0;
}
static int binding_matches(unsigned p, unsigned b, const struct device *d, unsigned type, unsigned code, int value)
{
    int binding = players[p].binding[b];
    if (binding < 0) return 0;
    if (type == 1) {
        if (directional(b)) {
            if (!value || !dpad_key_for_binding(b, code)) return 0;
            /* Linux drivers use both BTN_DPAD_* and the older KEY_UP/DOWN/
             * LEFT/RIGHT codes. A default map must accept either family. */
            if (binding) return code == (unsigned)binding;
            return !source_claimed(p, b, (int)code);
        }
        if (!value || code != binding_code(p, b, d)) return 0;
        return binding || !source_claimed(p, b, (int)code);
    }
    int direction = axis_direction(value);
    if (type != 3 || !direction || code >= AXES) return 0;
    if (directional(b) && !dpad_hat(code)) return 0;
    int source = AXIS_BIND(code, direction > 0);
    if (binding) return binding == source;
    return default_axis(b, code, direction) && !source_claimed(p, b, source);
}
static void raw_input_diagnostic(int i, unsigned type, unsigned code, int value,
                                 int axis_index, int normalized)
{
    int capture = capture_player >= 0 && players[capture_player].device == i;
    int test = test_player >= 0 && players[test_player].device == i;
    if (!capture && !test) return;
    static int key_state[DEVICES][KEYS];
    static unsigned char key_seen[DEVICES][KEYS];
    static signed char axis_state[DEVICES][AXES];
    static unsigned char axis_seen[DEVICES][AXES];
    int direction = axis_direction(normalized);
    if (type == 1 && code < KEYS) {
        int pressed = value != 0;
        if (key_seen[i][code] && key_state[i][code] == pressed) return;
        key_seen[i][code] = 1; key_state[i][code] = pressed;
    } else if (type == 3 && axis_index >= 0 && axis_index < AXES) {
        if (axis_seen[i][axis_index] && axis_state[i][axis_index] == direction) return;
        axis_seen[i][axis_index] = 1; axis_state[i][axis_index] = (signed char)direction;
    }
    unsigned target = capture ? (unsigned)capture_binding :
        test_step >= 0 && test_step < NG_INPUT_BINDINGS ? mapping_order[test_step] : NG_INPUT_BINDINGS;
    char line[192];
    snprintf(line, sizeof line,
        "input-raw js%d task=%s action=%s type=%u code=%u value=%d axis-index=%d normalized=%d armed=%d",
        i, capture ? "map" : "test", target < NG_INPUT_BINDINGS ? names[target] : "release",
        type, code, value, axis_index, normalized, capture ? capture_armed : !test_release);
    psx_diag(line);
}
static void test_event(int device, unsigned type, unsigned code, int value, int active)
{
    if (test_player < 0 || test_complete || test_step < 0 || test_release || !active ||
        players[test_player].device != device) return;
    unsigned target = mapping_order[test_step];
    int matches = binding_matches((unsigned)test_player, target, &devices[device], type, code, value);
    char diag[192];
    snprintf(test_detail, sizeof test_detail, "%s%s code %u value %d", matches ? "" : "Mismatch: ",
             type == 1 ? "Key" : "Axis", code, value);
    snprintf(diag, sizeof diag, "input-test player=%d step=%d action=%s type=%u code=%u value=%d match=%d source=%s",
             test_player + 1, test_step, names[target], type, code, value, matches,
             devices[device].ev_fd >= 0 ? "evdev" : "joydev");
    psx_diag(diag);
    /* A wrong control must be released, but never completes this prompt. */
    test_release = 1; test_wait_neutral = !matches; changed = 1;
}
static void capture_mapping(int i, int source)
{
    unsigned p = (unsigned)capture_player;
    /* Like mapping_clear_target + source assignment in NES/GB: replace the
     * target's previous key/axis and give this physical source one action. */
    for (unsigned b = 0; b < NG_INPUT_BINDINGS; ++b) {
        if (b == (unsigned)capture_binding) continue;
        if (players[p].binding[b] == source ||
            (source < AXIS_BINDING && !players[p].binding[b] &&
             binding_code(p, b, &devices[i]) == (unsigned)source)) players[p].binding[b] = -1;
    }
    players[p].binding[capture_binding] = source; sync_model(p);
    capture_feedback("Control captured");
    capture_player = -1; capture_armed = 0; capture_success = 1; devices[i].blocked = 1; changed = 1; save_config();
}
static void button_event(int i, unsigned code, int value, int initial)
{
    struct device *d = &devices[i];
    if (!code || code >= KEYS) return;
    d->key[code] = value != 0;
    if (initial) return;
    if (d->ev_fd < 0) { last_activity = poll_now; activity_valid = 1; }
    test_event(i, 1, code, value, value == 1);
    if (value == 1 && capture_player >= 0 && players[capture_player].device == i) {
        if (!capture_armed) capture_feedback("Release all controls first; mapping is not armed yet");
        else if (directional((unsigned)capture_binding) && !dpad_key(code))
            capture_feedback("D-pad step: use the D-pad, not a face button or analog stick");
        else capture_mapping(i, (int)code);
    }
}
static void axis_event(int i, unsigned a, int value, int initial)
{
    struct device *d = &devices[i];
    if (a >= d->axes) return;
    unsigned code = d->axis_code[a];
    if (code >= AXES) { d->axis[a] = value; return; }
    int direction = axis_direction(value);
    if (!initial && (direction || axis_direction(d->axis[a]))) {
        if (d->ev_fd < 0 && axis_used(d, a)) { last_activity = poll_now; activity_valid = 1; }
        test_event(i, 3, code, value, direction != 0);
        /* NES/GB only capture axes for the four directional prompts. */
        if (direction && capture_player >= 0 && players[capture_player].device == i) {
            if (!capture_armed) capture_feedback("Release all controls first; mapping is not armed yet");
            else if (directional((unsigned)capture_binding) && dpad_hat(code))
                capture_mapping(i, AXIS_BIND(code, direction > 0));
            else if (directional((unsigned)capture_binding))
                capture_feedback("D-pad step: use the physical D-pad; analog sticks stay separate");
            else capture_feedback("This step needs a button press; analog axes are ignored");
        }
    }
    d->axis[a] = value;
}
static unsigned long long input_monotonic_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 0;
    return (unsigned long long)ts.tv_sec * 1000000000ULL + (unsigned long long)ts.tv_nsec;
}

void psx_input_poll(unsigned long long now)
{
    poll_now = now;
    if (capture_paused) return;
    if (config_dirty && now >= next_config_retry) persist_config();
    if (now >= next_scan) {
        unsigned long long t0 = input_monotonic_ns();
        scan_devices();
        unsigned long long t1 = input_monotonic_ns();
        update_capture();
        unsigned long long t2 = input_monotonic_ns();
        next_scan = now + 1000000000ULL;
        unsigned long long scan_us = t1 >= t0 ? (t1 - t0) / 1000ULL : 0;
        unsigned long long capture_us = t2 >= t1 ? (t2 - t1) / 1000ULL : 0;
        if (scan_us >= 10000ULL || capture_us >= 5000ULL) {
            char line[128];
            snprintf(line, sizeof line, "input-scan scan_us=%llu capture_us=%llu",
                     scan_us, capture_us);
            psx_diag(line);
        }
    }
    if (capture_player >= 0 && now >= capture_until) psx_input_cancel_capture();
    for (int i = 0; i < DEVICES; ++i) {
        struct device *d = &devices[i]; if (d->fd < 0) continue;
        int drained = 0;
        for (unsigned budget = 0; budget < 64; ++budget) {
            if (d->ev_fd >= 0) {
                struct input_event32 e; long n = read(d->ev_fd, &e, sizeof e);
                if (n < 0 && errno == EAGAIN) { drained = 1; break; }
                if (n < 0 && errno == EINTR) continue;
                if (n != sizeof e) { drop_device(i); break; }
                if (e.type == 0 && e.code == 3) {
                    d->sync_lost = 1; d->blocked = 1;
                    if (capture_player >= 0 && players[capture_player].device == i) capture_armed = 0;
                    memset(d->key, 0, sizeof d->key); memset(d->axis, 0, sizeof d->axis);
                } else if (d->sync_lost) {
                    if (e.type == 0 && e.code == 0) {
                        resync_evdev(i, d);
                        d->sync_lost = 0; psx_diag("input evdev=resync-after-dropped-events");
                    }
                } else if (e.type == 1) {
                    raw_input_diagnostic(i, 1, e.code, e.value, -1, 0);
                    button_event(i, e.code, e.value, 0);
                } else if (e.type == 3) {
                    int found = 0;
                    for (unsigned a = 0; a < d->axes; ++a) {
                        if (d->axis_code[a] != e.code) continue;
                        found = 1;
                        int value = d->range_ok[a] ? normalized_axis(d, a, e.value) : e.value;
                        raw_input_diagnostic(i, 3, e.code, e.value, (int)a, value);
                        if (d->range_ok[a]) axis_event(i, a, value, 0);
                    }
                    if (!found) {
                        int a = register_evdev_hat_axis(i, d, e.code);
                        if (a >= 0) {
                            found = 1;
                            int value = normalized_axis(d, (unsigned)a, e.value);
                            raw_input_diagnostic(i, 3, e.code, e.value, a, value);
                            axis_event(i, (unsigned)a, value, 0);
                        }
                    }
                    if (!found) raw_input_diagnostic(i, 3, e.code, e.value, -1, e.value);
                }
            } else {
                struct js_event e; long n = read(d->fd, &e, sizeof e);
                if (n < 0 && errno == EAGAIN) { drained = 1; break; }
                if (n < 0 && errno == EINTR) continue;
                if (n != sizeof e) { drop_device(i); break; }
                unsigned type = e.type & 0x7f;
                if (type == 1 && e.number < d->buttons) button_event(i, d->button_code[e.number], e.value, e.type & 0x80);
                else if (type == 2 && e.number < d->js_axes) axis_event(i, e.number, e.value, e.type & 0x80);
            }
        }
        if (d->fd >= 0 && drained && !d->sync_lost && d->blocked && neutral(d, 1)) d->blocked = 0;
        if (capture_player >= 0 && players[capture_player].device == i && drained && !d->sync_lost && neutral(d, 1) && !capture_armed) {
            char ready[128];
            if (directional((unsigned)capture_binding))
                snprintf(ready, sizeof ready, "Armed: press physical %s", names[capture_binding]);
            else snprintf(ready, sizeof ready, "Armed: press %s", names[capture_binding]);
            capture_armed = 1; capture_feedback(ready);
        }
        if (test_player >= 0 && players[test_player].device == i && test_release && drained && !d->sync_lost && neutral(d, 1)) {
            test_release = 0;
            char diag[96]; snprintf(diag, sizeof diag, "input-test player=%d step=%d released", test_player + 1, test_step); psx_diag(diag);
            if (test_step < 0) { test_step = 0; test_wait_neutral = 0; }
            else if (test_wait_neutral) test_wait_neutral = 0;
            else if (++test_step >= NG_INPUT_BINDINGS) test_complete = 1;
            changed = 1;
        }
    }
    assign_devices();
    for (int d = 0; d < DEVICES; ++d) {
        uint16_t strong = 0, weak = 0;
        for (unsigned p = 0; p < 2; ++p) if (players[p].device == d) {
            uint16_t h = __atomic_load_n(&rumble_request[p][0], __ATOMIC_ACQUIRE);
            uint16_t l = __atomic_load_n(&rumble_request[p][1], __ATOMIC_ACQUIRE);
            if (h > strong) strong = h;
            if (l > weak) weak = l;
        }
        apply_rumble(&devices[d], strong, weak);
    }
}
int psx_input_recent_activity(unsigned long long now)
{
    if (capture_paused) return 0;
    if (activity_valid && now >= last_activity && now - last_activity <= 75000000ULL) return 1;
    /* Tizen can autorepeat aliases while joydev produces no new events. */
    for (int d = 0; d < DEVICES; ++d)
        if (devices[d].fd >= 0 && devices[d].ev_fd < 0 && !neutral(&devices[d], 1)) return 1;
    return 0;
}
uint16_t psx_input_buttons(unsigned p)
{
    if (p > 1 || players[p].device < 0) return 0;
    struct device *d = &devices[players[p].device]; uint16_t bits = 0;
    if (d->fd < 0 || d->blocked || capture_player >= 0 || capture_paused) return 0;
    for (unsigned b = 0; b < NG_INPUT_BINDINGS; ++b) {
        if (directional(b) && !players[p].binding[b]) {
            /* Default directional bindings accept any of the Linux D-pad key
             * aliases (KEY_UP or BTN_DPAD_UP, etc.). */
            for (unsigned key = 1; key < KEYS; ++key)
                if (d->key[key] && binding_matches(p, b, d, 1, key, 1)) bits |= masks[b];
        } else {
            unsigned key = binding_code(p, b, d);
            if (key && d->key[key] && binding_matches(p, b, d, 1, key, 1)) bits |= masks[b];
        }
        for (unsigned a = 0; a < d->axes; ++a)
            if (binding_matches(p, b, d, 3, d->axis_code[a], d->axis[a])) bits |= masks[b];
    }
    if ((bits & (NGP_LEFT | NGP_RIGHT)) == (NGP_LEFT | NGP_RIGHT)) bits &= ~(NGP_LEFT | NGP_RIGHT);
    if ((bits & (NGP_UP | NGP_DOWN)) == (NGP_UP | NGP_DOWN)) bits &= ~(NGP_UP | NGP_DOWN);
    return bits;
}
int16_t psx_input_analog(unsigned p, unsigned axis)
{
    static const unsigned linux_axis[] = {0, 1, 3, 4}; /* LX, LY, RX, RY */
    if (p > 1 || axis >= 4 || players[p].device < 0 || capture_paused || capture_player >= 0)
        return 0;
    struct device *d = &devices[players[p].device];
    if (d->fd < 0 || d->blocked) return 0;
    unsigned wanted = linux_axis[axis];
    for (unsigned a = 0; a < d->axes; ++a)
        if (d->axis_code[a] == wanted) return d->axis[a];
    return 0;
}
void psx_input_clear(void) { for (int i = 0; i < DEVICES; ++i) devices[i].blocked = 1; }
void psx_input_pause_capture(void)
{
    capture_paused = 1;
    psx_input_cancel_capture(); psx_input_test_cancel(); psx_input_clear();
    for (int i = 0; i < DEVICES; ++i)
        if (devices[i].fd >= 0) drop_device(i);
}
void psx_input_resume_capture(void)
{
    capture_paused = 0; activity_valid = 0; next_scan = 0;
    psx_input_clear();
}
void psx_input_shutdown(void) { if (config_dirty) persist_config(); for (int i = 0; i < DEVICES; ++i) if (devices[i].fd >= 0) drop_device(i); }
void psx_input_cycle_device(unsigned p, int direction)
{
    if (p > 1) return;
    int choices[DEVICES + 2], count = 2, current = -1;
    choices[0] = -1; choices[1] = -2;
    for (int d = 0; d < DEVICES; ++d) if (devices[d].fd >= 0) {
        int occupied = 0;
        for (unsigned other = 0; other < 2; ++other)
            if (other != p && players[other].device == d) occupied = 1;
        if (!occupied) choices[count++] = d;
    }
    if (!players[p].id[0]) current = 0;
    else if (!strcmp(players[p].id, "none")) current = 1;
    else for (int c = 2; c < count; ++c) if (!strcmp(players[p].id, devices[choices[c]].id)) { current = c; break; }
    int selected = (current + (direction < 0 ? -1 : 1) + count) % count;
    snprintf(players[p].id, ID_SIZE, "%s", selected == 0 ? "" : selected == 1 ? "none" : devices[choices[selected]].id);
    psx_input_cancel_capture(); psx_input_test_cancel(); assign_devices(); update_capture(); psx_input_clear(); save_config(); changed = 1;
}
void psx_input_device_label(unsigned p, char *out, size_t capacity)
{
    if (p > 1) { if (capacity) out[0] = 0; return; }
    int d = players[p].device;
    if (!strcmp(players[p].id, "none")) snprintf(out, capacity, "Disabled");
    else if (d >= 0) snprintf(out, capacity, "%s%s [js%d, %s]", players[p].id[0] ? "" : "Auto: ", devices[d].name, d, devices[d].ev_fd >= 0 ? "exclusive" : "shared");
    else snprintf(out, capacity, "%s", players[p].id[0] ? "Assigned device disconnected / in use" : "Auto: no readable gamepad");
}
const char *psx_input_binding_name(unsigned b) { return b < NG_INPUT_BINDINGS ? names[b] : ""; }
static const char *button_name(unsigned code)
{
    switch (code) {
    case 304: return "Cross / South";
    case 305: return "Circle / East";
    case 307: return "Triangle / North";
    case 308: return "Square / West";
    case 310: return "L1";
    case 311: return "R1";
    case 312: return "L2";
    case 313: return "R2";
    case 314: return "Select / Back";
    case 315: return "Start";
    case 317: return "L3";
    case 318: return "R3";
    case 103: case 544: return "D-pad Up";
    case 108: case 545: return "D-pad Down";
    case 105: case 546: return "D-pad Left";
    case 106: case 547: return "D-pad Right";
    default: return "Button";
    }
}
void psx_input_binding_label(unsigned p, unsigned b, char *out, size_t capacity)
{
    if (p > 1 || b >= NG_INPUT_BINDINGS) { if (capacity) out[0] = 0; return; }
    int code = players[p].binding[b], d = players[p].device;
    if (code < 0) snprintf(out, capacity, "Unbound");
    else if (code >= AXIS_BINDING) {
        unsigned axis = (unsigned)(code - AXIS_BINDING) / 2;
        snprintf(out, capacity, "%s %u %s (Linux ABS)", axis >= 16 && axis <= 23 ? "Hat axis" : "Axis",
                 axis, (code - AXIS_BINDING) & 1 ? "positive" : "negative");
    } else if (!code && directional(b)) snprintf(out, capacity, "Default: D-pad / hat");
    else {
        unsigned effective = d >= 0 ? binding_code(p, b, &devices[d]) : code ? (unsigned)code : defaults[b];
        snprintf(out, capacity, "%s%s (Linux %u)", code ? "" : "Default: ", button_name(effective), effective);
    }
}
void psx_input_reset(unsigned p) { if (p < 2) { memset(players[p].binding, 0, sizeof players[p].binding); psx_input_clear(); sync_model(p); save_config(); changed = 1; } }
void psx_input_unbind(unsigned p, unsigned b) { if (p < 2 && b < NG_INPUT_BINDINGS) { players[p].binding[b] = -1; psx_input_clear(); sync_model(p); save_config(); changed = 1; } }
void psx_input_capture(unsigned p, unsigned b, unsigned long long now)
{
    capture_success = 0;
    if (p > 1 || b >= NG_INPUT_BINDINGS || players[p].device < 0) return;
    capture_player = (int)p; capture_binding = (int)b; capture_armed = 0; capture_until = now + 30000000000ULL;
    snprintf(capture_feedback_text, sizeof capture_feedback_text,
             "Release all controls to arm %s", names[b]);
    psx_diag("input-map: capture started; waiting for neutral controls");
    psx_input_clear(); changed = 1;
}
void psx_input_cancel_capture(void) { capture_success = 0; if (capture_player >= 0) { capture_player = -1; psx_input_clear(); changed = 1; } }
int psx_input_capturing(void) { return capture_player >= 0; }
int psx_input_capture_succeeded(void) { return capture_success; }
int psx_input_changed(void) { int result = changed; changed = 0; return result; }

int psx_input_exclusive(void) { return exclusive_preferred; }
void psx_input_toggle_exclusive(void)
{
    exclusive_preferred = !exclusive_preferred; activity_valid = 0;
    psx_input_cancel_capture(); psx_input_test_cancel(); update_capture(); psx_input_clear(); save_config(); changed = 1;
}
const char *psx_input_capture_status(unsigned p)
{
    if (p < 2 && capture_player == (int)p) return capture_feedback_text;
    if (p > 1 || players[p].device < 0) return "No connected controller";
    return devices[players[p].device].capture_status[0] ? devices[players[p].device].capture_status :
        "waiting for device access; shared fallback";
}
void psx_input_test_begin(unsigned p)
{
    psx_input_cancel_capture(); test_player = p < 2 ? (int)p : 0;
    test_step = -1; test_release = 1; test_complete = 0; test_wait_neutral = 0; test_detail[0] = 0;
    psx_input_clear(); changed = 1; psx_diag("input-test begin (mappings unchanged)");
}
void psx_input_test_cancel(void)
{
    if (test_player >= 0) { test_player = -1; psx_input_clear(); changed = 1; }
}
void psx_input_test_skip(void)
{
    if (test_player < 0 || test_complete) return;
    char diag[96]; snprintf(diag, sizeof diag, "input-test player=%d step=%d skipped", test_player + 1, test_step); psx_diag(diag);
    if (++test_step >= NG_INPUT_BINDINGS) test_complete = 1;
    test_release = 1; test_wait_neutral = 1; test_detail[0] = 0; changed = 1;
}
void psx_input_test_label(char *out, size_t capacity)
{
    if (test_player < 0) snprintf(out, capacity, "Input test cancelled");
    else if (players[test_player].device < 0) snprintf(out, capacity, "Connect the selected Player %d controller", test_player + 1);
    else if (test_complete) snprintf(out, capacity, "Input test complete. Raw codes logged; mappings unchanged.");
    else if (test_step < 0) snprintf(out, capacity, "Release all buttons and directions to begin");
    else snprintf(out, capacity, "%d/%u: %s %s%s%s", test_step + 1, NG_INPUT_BINDINGS, test_release ? "Release" : "Press", names[mapping_order[test_step]],
                  test_detail[0] ? " | Last: " : "", test_detail);
}

#ifdef PSX_INPUT_SELFTEST
/* Run on the build host: cc -DPSX_INPUT_SELFTEST -Wall -Wextra -O1 -g
 * -fsanitize=address,undefined app/input.c -o /tmp/tizpsx-input-test
 * /tmp/tizpsx-input-test
 * Every syscall below is fake; no TV, device node or settings file is used. */
#include <stdarg.h>
static const char *test_config_input;
static char test_config_output[CONFIG_SIZE];
static size_t test_config_used;
static int test_write_failure, test_checks, test_failures;
static int test_ioctl_state, test_key_103;
static int test_fail_key_query, test_fail_abs_query;
static struct input_event32 test_events[8];
static unsigned test_event_count, test_event_next;
#define CHECK(expr) do { ++test_checks; if (!(expr)) { ++test_failures; \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); } } while (0)
int test_open(const char *path, int flags, ...)
{
    (void)flags;
    if (!strcmp(path, "input.cfg")) return 101;
    if (!strcmp(path, "input.cfg.tmp")) { test_config_used = 0; return 102; }
    if (!strcmp(path, ".")) return 103;
    errno = ENOENT; return -1;
}
long test_read(int fd, void *out, unsigned long capacity)
{
    if (fd == 101 && test_config_input) {
        size_t n = strlen(test_config_input); if (n > capacity) n = capacity;
        memcpy(out, test_config_input, n); return (long)n;
    }
    if (fd == 11 && test_event_next < test_event_count && capacity == sizeof test_events[0]) {
        memcpy(out, &test_events[test_event_next++], capacity); return (long)capacity;
    }
    errno = EAGAIN; return -1;
}
long test_write(int fd, const void *data, unsigned long size)
{
    if (test_write_failure || fd != 102 || size >= sizeof test_config_output - test_config_used) {
        errno = EIO; return -1;
    }
    memcpy(test_config_output + test_config_used, data, size); test_config_used += size;
    test_config_output[test_config_used] = 0; return (long)size;
}
int test_close(int fd) { (void)fd; return 0; }
int test_fsync(int fd) { (void)fd; return 0; }
int test_rename(const char *a, const char *b) { (void)a; (void)b; return 0; }
int test_ioctl(int fd, unsigned long op, ...)
{
    (void)fd;
    if (!test_ioctl_state) return -1;
    if ((op == EVIOCGKEY_STATE && test_fail_key_query) ||
        (op >= EVIOCGABS(0) && op < EVIOCGABS(AXES) && test_fail_abs_query)) {
        errno = EOPNOTSUPP; return -1;
    }
    va_list args; va_start(args, op);
    if (op == EVIOCGKEY_STATE) {
        uint8_t *keys = va_arg(args, uint8_t *); memset(keys, 0, 96);
        if (test_key_103) keys[103 >> 3] |= 1u << (103 & 7);
    } else if (op >= EVIOCGABS(0) && op < EVIOCGABS(AXES)) {
        struct absinfo *info = va_arg(args, struct absinfo *);
        memset(info, 0, sizeof *info); info->minimum = -32767; info->maximum = 32767;
    }
    va_end(args); return 0;
}
int test_access(const char *path, int mode) { (void)path; (void)mode; return -1; }
char *test_realpath(const char *path, char *out) { (void)path; (void)out; return 0; }
void psx_diag(const char *message) { (void)message; }
static void test_setup(const char *config)
{
    memset(devices, 0, sizeof devices); memset(players, 0, sizeof players); memset(models, 0, sizeof models);
    exclusive_preferred = 1; rumble_enabled = 1; model_limit = capture_paused = config_dirty = 0;
    test_event_count = test_event_next = 0; test_ioctl_state = test_key_103 = 0;
    test_fail_key_query = test_fail_abs_query = 0;
    test_player = capture_player = -1; capture_success = 0; config_error[0] = 0; poll_now = 0;
    test_config_input = config; psx_input_init("input.cfg"); next_scan = ~0ULL;
    devices[0].fd = 10; devices[0].ev_fd = -1; devices[0].blocked = 0;
    devices[0].axes = devices[0].js_axes = 7; devices[0].buttons = 7;
    devices[0].axis_code[0] = 0; devices[0].axis_code[1] = 1;
    devices[0].axis_code[2] = 3; devices[0].axis_code[3] = 4;
    devices[0].axis_code[4] = 5; /* Packed index differs from Linux ABS code. */
    devices[0].axis_code[5] = 16; devices[0].axis_code[6] = 17;
    for (unsigned b = 0; b < 7; ++b) devices[0].button_code[b] = defaults[b];
    players[0].device = 0; players[1].device = -1;
}
static void test_tick(void) { psx_input_poll(poll_now + 1000000ULL); }
static void test_neutral(void)
{
    memset(devices[0].key, 0, sizeof devices[0].key);
    memset(devices[0].axis, 0, sizeof devices[0].axis); test_tick();
}
int main(void)
{
    int value; unsigned b; char label[160], saved_config[CONFIG_SIZE];
    struct device fallback_map = {0};
    struct device evdev_hats = {0};
    CHECK(NG_INPUT_A == 0 && NG_INPUT_START == 4 && NG_INPUT_COIN == 6 && NG_INPUT_BINDINGS == 16);
    CHECK(psx_input_mapping_binding(14) == NG_INPUT_SELECT && psx_input_mapping_binding(15) == NG_INPUT_START);
    CHECK(psx_input_mapping_binding(16) == NG_INPUT_BINDINGS && psx_input_binding_mask(16) == 0);
    CHECK(parse_record("15=axis:5:-1", &b, &value) && b == NG_INPUT_RIGHT && value == AXIS_BIND(5, 0));
    CHECK(parse_record("12=key:103", &b, &value) && b == NG_INPUT_UP && value == 103);
    CHECK(parse_record("0=288", &b, &value) && value == 288);
    CHECK(parse_record("6=-1", &b, &value) && value == -1);
    CHECK(!parse_record("0=axis:5:+1", &b, &value));
    CHECK(!parse_record("15=axis:64:-1", &b, &value));
    CHECK(!parse_record("15=axis:5:-2", &b, &value));
    CHECK(!parse_record("16=304", &b, &value));
    CHECK(!parse_record("999999999999999999=304", &b, &value));
    CHECK(!parse_record("0=key:768", &b, &value));
    CHECK(!parse_record("0=key:0", &b, &value));
    CHECK(!parse_record("0=304junk", &b, &value));
    test_setup("rumble=0\n");
    CHECK(!psx_input_rumble_enabled() && !psx_input_rumble_request(0, 0, 32768));
    psx_input_set_rumble_enabled(1);
    CHECK(psx_input_rumble_enabled() && strstr(test_config_output, "rumble=1\n") != 0);
    CHECK(psx_input_rumble_request(0, 0, 32768));
    psx_input_rumble_stop();
    CHECK(!rumble_request[0][0] && !rumble_request[0][1] && !rumble_request[1][0] && !rumble_request[1][1]);
    fallback_map.axes = 8; fallback_map.buttons = 14;
    fallback_joydev_axis_map(&fallback_map); fallback_joydev_button_map(&fallback_map);
    CHECK(fallback_map.axis_code[0] == 0 && fallback_map.axis_code[1] == 1 &&
          fallback_map.axis_code[2] == 3 && fallback_map.axis_code[3] == 4 && fallback_map.axis_code[6] == 16 && fallback_map.axis_code[7] == 17);
    CHECK(fallback_map.button_code[0] == defaults[NG_INPUT_A] &&
          fallback_map.button_code[1] == defaults[NG_INPUT_B] &&
          fallback_map.button_code[8] == defaults[NG_INPUT_SELECT] &&
          fallback_map.button_code[9] == defaults[NG_INPUT_START] &&
          fallback_map.button_code[6] == defaults[NG_INPUT_COIN] &&
          fallback_map.button_code[13] == 317);
    evdev_hats.fd = 10; evdev_hats.ev_fd = 11; evdev_hats.axes = evdev_hats.js_axes = 4;
    evdev_hats.axis_code[0] = 0; evdev_hats.axis_code[1] = 1;
    evdev_hats.axis_code[2] = 3; evdev_hats.axis_code[3] = 4;
    CHECK(register_evdev_hat_axis(0, &evdev_hats, ABS_HAT0X) == 4);
    CHECK(register_evdev_hat_axis(0, &evdev_hats, ABS_HAT0Y) == 5);
    CHECK(evdev_hats.js_axes == 4 && evdev_hats.axes == 6 &&
          evdev_hats.axis_code[4] == ABS_HAT0X && evdev_hats.axis_code[5] == ABS_HAT0Y &&
          evdev_hats.range_ok[4] && evdev_hats.range_ok[5]);
    test_setup("p1.device=0003:1234:abcd:serial:test\np1.0=288\np1.6=-1\np1.15=axis:5:+1\n");
    CHECK(players[0].binding[0] == 288 && players[0].binding[6] == -1);
    CHECK(models[0].binding[0] == 288 && models[0].binding[15] == 0);
    CHECK(strstr(test_config_output, "p1.15=0\n") != 0);
    CHECK(strstr(test_config_output, "m.0003:1234:abcd.0=key:288\n") != 0);
    test_setup("p1.device=0003:1234:abcd:serial:test\np1.0=288\nm.0003:1234:abcd.0=305\nm.0003:1234:abcd.15=axis:5:-1\nexclusive=0\n");
    CHECK(exclusive_preferred == 0 && models[0].binding[0] == 305 && models[0].binding[15] == 0);
    snprintf(saved_config, sizeof saved_config, "%s", test_config_output); test_setup(saved_config);
    CHECK(exclusive_preferred == 0 && models[0].binding[0] == 305 && models[0].binding[15] == 0);
    CHECK(players[0].binding[0] == 288);
    snprintf(players[1].id, ID_SIZE, "0003:1234:abcd:serial:other");
    players[0].binding[NG_INPUT_UP] = 103; sync_model(0);
    CHECK(players[1].binding[NG_INPUT_UP] == 103 && models[0].binding[NG_INPUT_UP] == 103);
    test_setup("p1.device=auto\np1.0=288\np1.12=key:103\n");
    players[0].device = -1; devices[0].id[0] = 0; assign_devices();
    CHECK(players[0].device == 0 && players[0].id[0] == 0 && players[0].binding[0] == 288);
    test_setup("p1.device=auto\np1.0=288\np1.12=key:103\n");
    snprintf(devices[0].id, ID_SIZE, "0003:5678:abcd:serial:auto");
    players[0].device = -1; assign_devices();
    CHECK(players[0].binding[0] == 288 && models[0].binding[NG_INPUT_UP] == 103 && !migrate_auto_slot[0]);
    test_setup("");
    snprintf(devices[0].id, ID_SIZE, "0003:a5a5:5a5a:path:/sys/devices/virtual/input/input16");
    snprintf(devices[0].name, sizeof devices[0].name, "Tizen Virtual Gamepad");
    devices[1].fd = 12; snprintf(devices[1].id, ID_SIZE, "0003:0e6f:0188:serial:physical");
    snprintf(devices[1].name, sizeof devices[1].name, "USB Gamepad");
    players[0].device = players[1].device = -1;
    assign_devices(); CHECK(players[0].device == 1 && players[1].device == 0);
    test_setup("p1.device=0003:0e6f:0188:serial:pad\np2.device=0003:0e6f:0188:serial:pad\n");
    snprintf(devices[0].id, ID_SIZE, "0003:0e6f:0188:serial:pad");
    snprintf(devices[1].id, ID_SIZE, "0003:a5a5:5a5a:path:/sys/devices/virtual/input/input16");
    devices[1].fd = 12; assign_devices();
    CHECK(players[0].device == 0 && players[1].device == 1 &&
          !strcmp(players[1].id, devices[1].id));
    test_setup("");
    devices[0].axis[0] = -24000; devices[0].axis[2] = 18000;
    CHECK(psx_input_analog(0, 0) == -24000 && psx_input_analog(0, 2) == 18000);
    CHECK(psx_input_analog(0, 1) == 0 && psx_input_analog(0, 4) == 0);
    /* Stick movement remains analog only; digital directions use keys/HATs. */
    memset(devices[0].axis, 0, sizeof devices[0].axis);
    for (b = 0; b < NG_INPUT_BINDINGS; ++b) {
        devices[0].key[defaults[b]] = 1; CHECK(psx_input_buttons(0) == masks[b]); devices[0].key[defaults[b]] = 0;
    }
    devices[0].key[103] = 1; CHECK(psx_input_buttons(0) == NGP_UP); devices[0].key[103] = 0;
    devices[0].key[108] = 1; CHECK(psx_input_buttons(0) == NGP_DOWN); devices[0].key[108] = 0;
    devices[0].key[105] = 1; CHECK(psx_input_buttons(0) == NGP_LEFT); devices[0].key[105] = 0;
    devices[0].key[106] = 1; CHECK(psx_input_buttons(0) == NGP_RIGHT); devices[0].key[106] = 0;
    axis_event(0, 0, -32767, 0); CHECK(psx_input_buttons(0) == 0);
    axis_event(0, 2, 32767, 0); CHECK(psx_input_buttons(0) == 0); /* Analog sticks are independent. */
    axis_event(0, 5, -32767, 0); CHECK(psx_input_buttons(0) == NGP_LEFT);
    test_neutral(); axis_event(0, 4, 32767, 0); CHECK(psx_input_buttons(0) == 0); test_neutral();
    /* Held control cannot become a fresh mapping, nor can Player 2 input. */
    button_event(0, 304, 1, 0); psx_input_capture(0, NG_INPUT_UP, poll_now); test_tick();
    CHECK(!capture_armed && !psx_input_capture_succeeded()); button_event(0, 103, 1, 0); CHECK(psx_input_capturing());
    test_neutral(); CHECK(capture_armed);
    button_event(1, 103, 1, 0); CHECK(psx_input_capturing());
    button_event(0, 103, 1, 0); CHECK(!psx_input_capturing() && psx_input_capture_succeeded() && players[0].binding[NG_INPUT_UP] == 103);
    test_neutral(); button_event(0, 103, 1, 0); CHECK(psx_input_buttons(0) == NGP_UP);
    test_neutral(); axis_event(0, 1, -32767, 0); CHECK(psx_input_buttons(0) == 0); test_neutral();
    /* Analog axes cannot be captured as digital directions; HAT signs can. */
    psx_input_capture(0, NG_INPUT_LEFT, poll_now); CHECK(!psx_input_capture_succeeded()); test_tick(); axis_event(0, 4, -32767, 0);
    CHECK(psx_input_capturing() && strstr(psx_input_capture_status(0), "D-pad step") != 0);
    test_neutral(); axis_event(0, 5, -32767, 0);
    CHECK(players[0].binding[NG_INPUT_LEFT] == AXIS_BIND(16, 0));
    test_neutral(); psx_input_capture(0, NG_INPUT_LEFT, poll_now); test_tick(); axis_event(0, 5, -32767, 0);
    CHECK(players[0].binding[NG_INPUT_LEFT] == AXIS_BIND(16, 0));
    test_neutral(); psx_input_capture(0, NG_INPUT_RIGHT, poll_now); test_tick(); axis_event(0, 5, 32767, 0);
    CHECK(players[0].binding[NG_INPUT_RIGHT] == AXIS_BIND(16, 1));
    test_neutral(); axis_event(0, 5, -32767, 0); CHECK(psx_input_buttons(0) == NGP_LEFT);
    test_neutral(); axis_event(0, 5, 32767, 0); CHECK(psx_input_buttons(0) == NGP_RIGHT); test_neutral();
    psx_input_binding_label(0, NG_INPUT_RIGHT, label, sizeof label); CHECK(strstr(label, "Hat axis 16 positive") != 0);
    /* Action captures ignore axes, and EV_KEY codes below BTN_MISC are valid. */
    psx_input_capture(0, NG_INPUT_A, poll_now); test_tick(); axis_event(0, 0, -32767, 0);
    CHECK(psx_input_capturing()); test_neutral(); button_event(0, 30, 1, 0);
    CHECK(players[0].binding[NG_INPUT_A] == 30); test_neutral(); button_event(0, 30, 1, 0);
    CHECK(psx_input_buttons(0) == NGP_A); test_neutral();
    /* Reusing a source moves its action (NES/GB source-assignment behavior). */
    psx_input_capture(0, NG_INPUT_B, poll_now); test_tick(); button_event(0, 30, 1, 0);
    CHECK(players[0].binding[NG_INPUT_A] == -1 && players[0].binding[NG_INPUT_B] == 30); test_neutral();
    psx_input_unbind(0, NG_INPUT_LEFT); axis_event(0, 0, -32767, 0); CHECK(psx_input_buttons(0) == 0); test_neutral();
    psx_input_reset(0); CHECK(players[0].binding[NG_INPUT_LEFT] == 0);
    /* All sixteen test prompts accept their own mapping and reject a wrong key. */
    psx_input_test_begin(0); test_tick(); CHECK(test_step == 0);
    for (unsigned step = 0; step < NG_INPUT_BINDINGS; ++step) {
        unsigned target = psx_input_mapping_binding(step);
        button_event(0, 42, 1, 0); CHECK(test_release && test_wait_neutral);
        test_neutral(); CHECK(test_step == (int)step && !test_release);
        button_event(0, defaults[target], 1, 0); CHECK(test_release && !test_wait_neutral);
        test_neutral(); CHECK(test_step == (int)step + 1);
    }
    CHECK(test_complete); psx_input_test_cancel();
    /* The guided test must accept KEY_UP as well as BTN_DPAD_UP on default maps. */
    psx_input_reset(0); psx_input_test_begin(0); test_tick(); CHECK(test_step == 0);
    button_event(0, 103, 1, 0); CHECK(test_release && !test_wait_neutral);
    test_neutral(); CHECK(test_step == 1); psx_input_test_cancel();
    /* Axis sign must match; release after skip cannot complete the next prompt. */
    players[0].binding[NG_INPUT_UP] = AXIS_BIND(17, 0);
    psx_input_test_begin(0); test_tick(); axis_event(0, 6, 32767, 0); test_neutral(); CHECK(test_step == 0);
    axis_event(0, 6, -32767, 0); test_neutral(); CHECK(test_step == 1);
    psx_input_test_skip(); CHECK(test_step == 2); test_neutral(); CHECK(test_step == 2);
    psx_input_test_cancel();
    players[0].binding[NG_INPUT_UP] = AXIS_BIND(17, 0);
    psx_input_test_begin(0); test_tick(); axis_event(0, 6, 32767, 0); test_neutral(); CHECK(test_step == 0);
    axis_event(0, 6, -32767, 0); test_neutral(); CHECK(test_step == 1); psx_input_test_cancel();
    /* Persistence failure remains dirty and a later retry saves typed values. */
    test_write_failure = 1; psx_input_unbind(0, NG_INPUT_COIN); CHECK(config_dirty && psx_input_error()[0]);
    test_write_failure = 0; psx_input_poll(next_config_retry); CHECK(!config_dirty && !psx_input_error()[0]);
    psx_input_capture(0, NG_INPUT_A, poll_now); psx_input_poll(capture_until); CHECK(!psx_input_capturing() && !psx_input_capture_succeeded());
    psx_input_capture(0, NG_INPUT_A, poll_now); psx_input_cancel_capture(); CHECK(!psx_input_capture_succeeded());
    /* Dropped evdev events resync lower EV_KEY codes and all mapped axes,
     * discard intervening events and hold output until neutral. */
    players[0].binding[NG_INPUT_UP] = 103; players[0].binding[NG_INPUT_LEFT] = AXIS_BIND(5, 0);
    devices[0].ev_fd = 11; test_ioctl_state = test_key_103 = 1;
    CHECK(resync_evdev(0, &devices[0]) && devices[0].key[103] && devices[0].range_ok[4]);
    /* Driver state-query ioctls may fail even while EVIOCGRAB/event reads work.
     * Keep the grab, start neutral, and use a signed range for future axes. */
    memset(devices[0].key, 0xff, sizeof devices[0].key);
    devices[0].range_ok[4] = 0; devices[0].axis[4] = 22000;
    test_fail_key_query = test_fail_abs_query = 1;
    CHECK(resync_evdev(0, &devices[0]) && devices[0].warned_key_query && devices[0].warned_axis_query);
    CHECK(devices[0].key[103] == 0 && devices[0].axis[4] == 0 && devices[0].range_ok[4] &&
          devices[0].minimum[4] == -32767 && devices[0].maximum[4] == 32767);
    devices[0].axes = 8; devices[0].axis_code[6] = 16; devices[0].range_ok[6] = 0;
    CHECK(resync_evdev(0, &devices[0]) && devices[0].minimum[6] == -1 && devices[0].maximum[6] == 1);
    CHECK(normalized_axis(&devices[0], 6, -1) == -32767 && normalized_axis(&devices[0], 6, 1) == 32767);
    test_fail_key_query = test_fail_abs_query = 0;
    CHECK(resync_evdev(0, &devices[0]) && devices[0].key[103]);
    test_events[0] = (struct input_event32){0, 0, 0, 3, 0};
    test_events[1] = (struct input_event32){0, 0, 1, 305, 1};
    test_events[2] = (struct input_event32){0, 0, 0, 0, 0};
    test_event_count = 3; test_event_next = 0; test_tick();
    CHECK(!devices[0].sync_lost && devices[0].blocked && !devices[0].key[305] && devices[0].key[103]);
    CHECK(psx_input_buttons(0) == 0);
    test_key_103 = 0; test_event_count = 1; test_event_next = 0;
    test_events[0] = (struct input_event32){0, 0, 1, 103, 0}; test_tick();
    CHECK(!devices[0].blocked && psx_input_buttons(0) == 0);
    psx_input_capture(0, NG_INPUT_A, poll_now); test_tick();
    test_events[0] = (struct input_event32){0, 0, 1, 103, 1}; test_event_count = 1; test_event_next = 0; test_tick();
    CHECK(players[0].binding[NG_INPUT_A] == 103 && !psx_input_capturing());
    test_event_count = test_event_next = 0; psx_input_pause_capture();
    CHECK(capture_paused && devices[0].fd == -1 && devices[0].ev_fd == -1);
    psx_input_resume_capture(); CHECK(!capture_paused && !next_scan);
    printf("TizPSX input: %d checks, %d failures (mock I/O; no device access)\n", test_checks, test_failures);
    return test_failures != 0;
}
#endif
