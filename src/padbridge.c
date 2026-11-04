// padbridge: presents an 8BitDo Ultimate C 2.4G (D-input, 2dc8:3016) to Wine as a
// USB DualSense, with rumble.
//
// Wine's macOS HID bus (dlls/winebus.sys/bus_iohid.c) learns a device from
// IOHIDDeviceGetProperty, reads it through IOHIDDeviceRegisterInputReportCallback
// and writes it through IOHIDDeviceSetReport/GetReport. Wine hands DualSense IDs
// (054c:0ce6) to games as raw HID, which is how games with their own PlayStation
// code (Kingdom Come: Deliverance II among them) find a pad. This library is
// inserted into the game's Wine processes and interposes those IOKit calls for
// the 8BitDo only: it answers with DualSense properties and a DualSense report
// descriptor, turns each 8BitDo report into a DualSense input report, and turns
// the game's DualSense rumble into the 8BitDo's motor report.
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDManager.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ds5.h"

#define PB_VERSION "0.1.0"

#define DYLD_INTERPOSE(replacement, replacee)                                              \
    __attribute__((used)) static struct {                                                  \
        const void *r;                                                                     \
        const void *e;                                                                     \
    } interpose_##replacee __attribute__((section("__DATA,__interpose"))) = {              \
        (const void *)(unsigned long)&replacement, (const void *)(unsigned long)&replacee}

#pragma mark - settings

enum { MAP_NONE, MAP_CREATE, MAP_TOUCHPAD, MAP_MUTE, MAP_PS };

static struct {
    int enabled;
    int log;             // 0 off, 1 events, 2 also every changed input report
    int view;            // what View sends
    int extra3, extra6;  // HID buttons 3 and 6 (unused on most 8BitDo D-input pads)
    int strength;        // rumble strength in percent
    int max;             // soft ceiling for strong rumble, 1-255 (255: none)
    int floor;           // lowest motor level for a nonzero request, 0-255
    int release_ms;      // how long a falling level takes to fade out
    int swap_motors;
} cfg = {1, 1, MAP_TOUCHPAD, MAP_CREATE, MAP_MUTE, 100, 180, 0, 30, 0};

static int pb_log_fd = -1;
static pthread_mutex_t pb_log_mu = PTHREAD_MUTEX_INITIALIZER;

static void pb_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void pb_log(const char *fmt, ...) {
    if (!cfg.log || pb_log_fd < 0) return;
    char line[1024];
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    int n = snprintf(line, sizeof line, "%02d:%02d:%02d.%03ld [%d %s] ", tm.tm_hour, tm.tm_min,
                     tm.tm_sec, ts.tv_nsec / 1000000, getpid(), getprogname());
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(line + n, sizeof line - n - 1, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof line - 2) n = sizeof line - 2;
    line[n++] = '\n';
    pthread_mutex_lock(&pb_log_mu);
    write(pb_log_fd, line, n);
    pthread_mutex_unlock(&pb_log_mu);
}

static int parse_map(const char *v) {
    if (!strcmp(v, "create")) return MAP_CREATE;
    if (!strcmp(v, "touchpad")) return MAP_TOUCHPAD;
    if (!strcmp(v, "mute")) return MAP_MUTE;
    if (!strcmp(v, "ps")) return MAP_PS;
    return MAP_NONE;
}

// ~/.config/padbridge/padbridge.conf, "key = value" lines, # comments.
static void load_config(void) {
    const char *home = getenv("HOME");
    if (!home) return;
    char path[1024];
    snprintf(path, sizeof path, "%s/.config/padbridge/padbridge.conf", home);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char key[64], val[128];
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        if (sscanf(line, " %63[a-z_0-9] = %127s", key, val) != 2) continue;
        int num = atoi(val);
        if (!strcmp(key, "enabled")) cfg.enabled = num != 0;
        else if (!strcmp(key, "log")) cfg.log = num;
        else if (!strcmp(key, "view")) cfg.view = parse_map(val);
        else if (!strcmp(key, "button3")) cfg.extra3 = parse_map(val);
        else if (!strcmp(key, "button6")) cfg.extra6 = parse_map(val);
        else if (!strcmp(key, "rumble_strength")) cfg.strength = num < 0 ? 0 : num > 200 ? 200 : num;
        else if (!strcmp(key, "rumble_max")) cfg.max = num < 1 ? 1 : num > 255 ? 255 : num;
        else if (!strcmp(key, "rumble_floor")) cfg.floor = num < 0 ? 0 : num > 200 ? 200 : num;
        else if (!strcmp(key, "rumble_release_ms")) cfg.release_ms = num < 0 ? 0 : num > 500 ? 500 : num;
        else if (!strcmp(key, "swap_motors")) cfg.swap_motors = num != 0;
    }
    fclose(f);
}

static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

#pragma mark - devices

typedef struct pad {
    IOHIDDeviceRef dev;            // the real 8BitDo, as Wine holds it
    int wanted;                    // 1 the 8BitDo, 0 something else (cached)
    // Wine's input subscription
    IOHIDReportCallback wine_cb;
    void *wine_ctx;
    uint8_t *wine_buf;
    CFIndex wine_len;
    uint8_t real_buf[64];
    // what the 8BitDo last said
    uint8_t in[10];
    int have_in;
    uint8_t last_logged[10];
    // DualSense report state
    uint8_t counter;
    uint32_t sequence;
    uint64_t started_ns, last_emit_ns;
    CFRunLoopRef loop;
    CFStringRef loop_mode;
    CFRunLoopTimerRef pump;
    // rumble, set by the game: 0-255
    uint8_t want_left, want_right;
    struct pad *next;
} pad;

static pthread_mutex_t pads_mu = PTHREAD_MUTEX_INITIALIZER;
static pad *pads;

// Every property the bus reads, answered for the 8BitDo with a DualSense's.
static CFNumberRef num_vid, num_pid, num_version, num_in_len, num_out_len, num_feat_len;
static CFDataRef data_descriptor;
static CFStringRef str_product, str_manufacturer;

static CFNumberRef make_num(int v) { return CFNumberCreate(NULL, kCFNumberIntType, &v); }

static int prop_int(IOHIDDeviceRef dev, CFStringRef key) {
    int v = 0;
    CFTypeRef n = IOHIDDeviceGetProperty(dev, key);
    if (n && CFGetTypeID(n) == CFNumberGetTypeID()) CFNumberGetValue(n, kCFNumberIntType, &v);
    return v;
}

static int is_8bitdo(IOHIDDeviceRef dev) {
    // These calls go to the real IOKit: interposing never applies inside this image.
    return prop_int(dev, CFSTR(kIOHIDVendorIDKey)) == 0x2dc8 &&
           prop_int(dev, CFSTR(kIOHIDProductIDKey)) == 0x3016;
}

// The pad for dev, made on first sight. NULL when dev is not the 8BitDo.
static pad *pad_for(IOHIDDeviceRef dev, int create) {
    if (!dev || !cfg.enabled) return NULL;
    pthread_mutex_lock(&pads_mu);
    pad *p = pads;
    while (p && p->dev != dev) p = p->next;
    if (!p && create) {
        p = calloc(1, sizeof *p);
        p->dev = dev;
        p->wanted = is_8bitdo(dev);
        p->next = pads;
        pads = p;
        if (p->wanted) {
            CFRetain(dev);
            p->started_ns = now_ns();
            pb_log("8BitDo %p found, presenting it as a DualSense", (void *)dev);
        }
    }
    pthread_mutex_unlock(&pads_mu);
    return p && p->wanted ? p : NULL;
}

#pragma mark - input: 8BitDo report to DualSense report

static inline int btn(const uint8_t *in, int n) { return ((in[1] | in[2] << 8) >> (n - 1)) & 1; }

static void apply_map(int map, uint8_t *b1, uint8_t *b2) {
    switch (map) {
    case MAP_CREATE: *b1 |= 0x10; break;
    case MAP_TOUCHPAD: *b2 |= 0x02; break;
    case MAP_MUTE: *b2 |= 0x04; break;
    case MAP_PS: *b2 |= 0x01; break;
    }
}

static void put16(uint8_t *p, int v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = v >> (8 * i); }

static void build_ds5(pad *p, uint8_t *r) {
    const uint8_t *in = p->in;
    memset(r, 0, DS5_INPUT_LEN);
    r[0] = 0x01;
    if (p->have_in) {
        r[DS5_IN_LX] = in[4];
        r[DS5_IN_LY] = in[5];
        r[DS5_IN_RX] = in[6];
        r[DS5_IN_RY] = in[7];
        r[DS5_IN_L2] = in[9];  // Brake
        r[DS5_IN_R2] = in[8];  // Accelerator
    } else {
        r[DS5_IN_LX] = r[DS5_IN_LY] = r[DS5_IN_RX] = r[DS5_IN_RY] = 0x80;
    }
    r[DS5_IN_COUNTER] = p->counter++;

    int hat = p->have_in ? in[3] & 0x0f : 8;
    if (hat > 7) hat = 8;
    uint8_t b0 = hat, b1 = 0, b2 = 0;
    if (p->have_in) {
        if (btn(in, 4)) b0 |= 0x10;   // X      -> Square
        if (btn(in, 1)) b0 |= 0x20;   // A      -> Cross
        if (btn(in, 2)) b0 |= 0x40;   // B      -> Circle
        if (btn(in, 5)) b0 |= 0x80;   // Y      -> Triangle
        if (btn(in, 7)) b1 |= 0x01;   // LB     -> L1
        if (btn(in, 8)) b1 |= 0x02;   // RB     -> R1
        if (in[9] > 2) b1 |= 0x04;    // LT     -> L2 (digital)
        if (in[8] > 2) b1 |= 0x08;    // RT     -> R2 (digital)
        if (btn(in, 12)) b1 |= 0x20;  // Menu   -> Options
        if (btn(in, 14)) b1 |= 0x40;  // L3
        if (btn(in, 15)) b1 |= 0x80;  // R3
        if (btn(in, 13)) b2 |= 0x01;  // Home   -> PS
        if (btn(in, 11)) apply_map(cfg.view, &b1, &b2);
        if (btn(in, 3)) apply_map(cfg.extra3, &b1, &b2);
        if (btn(in, 6)) apply_map(cfg.extra6, &b1, &b2);
    }
    r[DS5_IN_BUTTONS0] = b0;
    r[DS5_IN_BUTTONS1] = b1;
    r[DS5_IN_BUTTONS2] = b2;

    uint64_t t_us = (now_ns() - p->started_ns) / 1000;
    put32(r + DS5_IN_SEQUENCE, p->sequence++);
    // No motion sensors on this pad: still, level, gravity on +Y.
    put16(r + DS5_IN_ACCEL + 2, 8192);
    put32(r + DS5_IN_SENSOR_TIME, (uint32_t)(t_us * 3));  // 0.33 us units
    r[DS5_IN_TEMP] = 0x1a;
    r[DS5_IN_TOUCH1] = 0x80;  // no finger on the touchpad
    r[DS5_IN_TOUCH2] = 0x80;
    put32(r + DS5_IN_TIMER2, (uint32_t)t_us);
    r[DS5_IN_BATTERY] = 0x2a;  // wired, full
    r[DS5_IN_CONNECT] = 0x08;  // USB
}

// Wine's callback runs on Wine's own HID thread (the device's run loop), as it
// would for a real DualSense.
static void emit(pad *p, IOReturn result) {
    if (!p->wine_cb || !p->wine_buf || p->wine_len < DS5_INPUT_LEN) return;
    build_ds5(p, p->wine_buf);
    p->last_emit_ns = now_ns();
    p->wine_cb(p->wine_ctx, result, p->dev, kIOHIDReportTypeInput, 0x01, p->wine_buf, DS5_INPUT_LEN);
}

static void log_input(pad *p) {
    if (cfg.log < 2 || !memcmp(p->in, p->last_logged, sizeof p->in)) return;
    memcpy(p->last_logged, p->in, sizeof p->in);
    const uint8_t *in = p->in;
    char pressed[64] = "";
    for (int n = 1; n <= 15; n++)
        if (btn(in, n)) snprintf(pressed + strlen(pressed), sizeof pressed - strlen(pressed), " b%d", n);
    pb_log("8BitDo in: hat %d  L %3d,%3d  R %3d,%3d  LT %3d RT %3d  buttons:%s", in[3] & 0x0f, in[4],
           in[5], in[6], in[7], in[9], in[8], pressed[0] ? pressed : " -");
}

static void on_real_report(void *ctx, IOReturn result, void *sender, IOHIDReportType type,
                           uint32_t id, uint8_t *report, CFIndex len) {
    (void)sender; (void)type;
    pad *p = ctx;
    // Report 1 is the pad state; the receiver's vendor report 2 has nothing for us.
    if (id != 0x01) return;
    if (len >= 10 && report[0] == 0x01) memcpy(p->in, report, 10);
    else if (len == 9) { p->in[0] = 0x01; memcpy(p->in + 1, report, 9); }
    else return;
    p->have_in = 1;
    log_input(p);
    emit(p, result);
}

// A DualSense streams at 250 Hz whether or not anything changed, and PlayStation
// code can take silence for a disconnect. The 8BitDo goes quiet when idle, so
// repeat the last state between its reports.
static void on_pump(CFRunLoopTimerRef t, void *ctx) {
    (void)t;
    pad *p = ctx;
    if (now_ns() - p->last_emit_ns >= 3500000ull) emit(p, kIOReturnSuccess);
}

static void pump_start(pad *p) {
    if (p->pump || !p->loop) return;
    CFRunLoopTimerContext tc = {0, p, NULL, NULL, NULL};
    p->pump = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 0.004, 0.004, 0, 0, on_pump, &tc);
    CFRunLoopAddTimer(p->loop, p->pump, p->loop_mode ? p->loop_mode : kCFRunLoopDefaultMode);
}

static void pump_stop(pad *p) {
    if (!p->pump) return;
    CFRunLoopTimerInvalidate(p->pump);
    CFRelease(p->pump);
    p->pump = NULL;
}

#pragma mark - rumble

// The receiver's vendor output report 0x81: 81 11 04 08, a little-endian duration
// in ms, then left and right strength 0-255. It stops the motors itself when the
// duration runs out, so a held level is written again before then.
static pthread_mutex_t rumble_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t rumble_cv = PTHREAD_COND_INITIALIZER;
static pad *rumble_pad;
static int rumble_dirty;

static int send_motors(IOHIDDeviceRef dev, uint8_t left, uint8_t right) {
    uint8_t report[64] = {0x81, 0x11, 0x04, 0x08, 0, 0, left, right};
    if (left || right) put16(report + 4, 200);
    return IOHIDDeviceSetReport(dev, kIOHIDReportTypeOutput, 0x81, report, sizeof report);
}

// Game level 0-255 to motor level 0-1, after strength, ceiling and floor.
// The motors are felt from level 4 up, so low levels pass through as they are.
// Near full power they get harsh, so strong levels bend smoothly towards
// rumble_max (tanh: linear at the bottom, flat at the top) instead of clipping.
static double shape(uint8_t v) {
    if (!v) return 0;
    double x = v / 255.0 * cfg.strength / 100.0;
    if (cfg.max < 255) {
        double m = cfg.max / 255.0;
        x = m * tanh(x / m);
    }
    if (x > 1) x = 1;
    double lo = cfg.floor / 255.0;
    return lo + x * (1 - lo);
}

static void *rumble_thread(void *arg) {
    (void)arg;
    double cur_l = 0, cur_r = 0;
    uint8_t sent_l = 0, sent_r = 0;
    uint64_t sent_at = 0;
    int failures = 0;
    pthread_mutex_lock(&rumble_mu);
    for (;;) {
        pad *p = rumble_pad;
        double want_l = p ? shape(cfg.swap_motors ? p->want_right : p->want_left) : 0;
        double want_r = p ? shape(cfg.swap_motors ? p->want_left : p->want_right) : 0;
        rumble_dirty = 0;
        IOHIDDeviceRef dev = p ? p->dev : NULL;
        if (dev) CFRetain(dev);
        pthread_mutex_unlock(&rumble_mu);

        // Rises land at once. Falls fade over release_ms: an ERM motor coasts
        // anyway, and a short fade keeps quick pulses from chattering.
        double step = cfg.release_ms > 0 ? 8.0 / cfg.release_ms : 1;
        cur_l = want_l >= cur_l ? want_l : fmax(want_l, cur_l - step);
        cur_r = want_r >= cur_r ? want_r : fmax(want_r, cur_r - step);
        uint8_t out_l = (uint8_t)lrint(cur_l * 255), out_r = (uint8_t)lrint(cur_r * 255);
        uint64_t now = now_ns();
        int changed = out_l != sent_l || out_r != sent_r;
        int refresh = (out_l || out_r) && now - sent_at >= 60000000ull;
        // Games update rumble at up to 250 Hz. One motor report per 8 ms is as
        // fast as the motors follow, and keeps the 2.4 GHz link free for input.
        int too_soon = now - sent_at < 8000000ull;
        if (dev && (changed || refresh) && !too_soon) {
            IOReturn r = send_motors(dev, out_l, out_r);
            if (r == kIOReturnSuccess) {
                if (changed && cfg.log >= 2) pb_log("rumble %3d %3d", out_l, out_r);
                sent_l = out_l, sent_r = out_r, sent_at = now;
                failures = 0;
            } else if (failures++ < 3) {
                pb_log("motor report failed: %08x", r);
            }
        }
        if (dev) CFRelease(dev);

        pthread_mutex_lock(&rumble_mu);
        int fading = fabs(cur_l - want_l) > 1e-6 || fabs(cur_r - want_r) > 1e-6;
        int pending = dev && changed && too_soon;
        if (!rumble_dirty || pending) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            uint64_t wait_ns = pending ? 8000000ull - (now - sent_at)
                               : fading ? 8000000ull
                               : (out_l || out_r) ? 60000000ull : 0;
            if (wait_ns) {
                ts.tv_nsec += wait_ns % 1000000000ull;
                ts.tv_sec += wait_ns / 1000000000ull + ts.tv_nsec / 1000000000;
                ts.tv_nsec %= 1000000000;
                pthread_cond_timedwait(&rumble_cv, &rumble_mu, &ts);
            } else {
                pthread_cond_wait(&rumble_cv, &rumble_mu);
            }
        }
    }
    return NULL;
}

static void rumble_start(void) {
    pthread_t th;
    if (pthread_create(&th, NULL, rumble_thread, NULL) == 0) pthread_detach(th);
}

static void rumble_set(pad *p, uint8_t left, uint8_t right) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, rumble_start);
    pthread_mutex_lock(&rumble_mu);
    if (p) {
        p->want_left = left;
        p->want_right = right;
        rumble_pad = p;
    }
    rumble_dirty = 1;
    pthread_cond_signal(&rumble_cv);
    pthread_mutex_unlock(&rumble_mu);
}

static void handle_ds5_output(pad *p, const uint8_t *r, CFIndex len) {
    if (len < 5 || r[0] != 0x02) return;
    uint8_t f0 = r[DS5_OUT_FLAGS0];
    uint8_t f2 = len > DS5_OUT_FLAGS2 ? r[DS5_OUT_FLAGS2] : 0;
    // The motor bytes count when the report turns rumble emulation on. A report
    // without those bits hands the motors back to audio haptics, which this pad
    // doesn't have: that's silence.
    int rumble = (f0 & 0x03) || (f2 & 0x04);
    uint8_t left = rumble ? r[DS5_OUT_RUMBLE_LEFT] : 0;
    uint8_t right = rumble ? r[DS5_OUT_RUMBLE_RIGHT] : 0;
    if (cfg.log >= 2 && (left != p->want_left || right != p->want_right))
        pb_log("game output: flags %02x/%02x/%02x  left %3d right %3d", f0, r[DS5_OUT_FLAGS1], f2,
               r[DS5_OUT_RUMBLE_LEFT], r[DS5_OUT_RUMBLE_RIGHT]);
    rumble_set(p, left, right);
}

#pragma mark - feature reports

static CFIndex ds5_feature(pad *p, uint8_t id, uint8_t *buf, CFIndex cap) {
    uint8_t r[DS5_FEATURE_LEN] = {id};
    CFIndex len = DS5_FEATURE_LEN;
    switch (id) {
    case 0x05: {  // calibration: zero bias, the ranges SDL and hid-playstation expect
        const int16_t v[] = {0, 0, 0, 8640, -8640, 8640, -8640, 8640, -8640,
                             540, 540, 8192, -8192, 8192, -8192, 8192, -8192};
        for (unsigned i = 0; i < sizeof v / sizeof *v; i++) put16(r + 1 + 2 * i, v[i]);
        len = 41;
        break;
    }
    case 0x09: {  // pairing info: a Bluetooth address, made from the pad's location
        uint32_t loc = (uint32_t)prop_int(p->dev, CFSTR(kIOHIDLocationIDKey));
        const uint8_t mac[6] = {loc & 0xff, (loc >> 8) & 0xff, (loc >> 16) & 0xff, 0xdc, 0x8b, 0x2d};
        memcpy(r + 1, mac, 6);
        len = 20;
        break;
    }
    case 0x20:
        memcpy(r, ds5_feature_firmware, sizeof ds5_feature_firmware);
        break;
    }
    if (len > cap) len = cap;
    memcpy(buf, r, len);
    return len;
}

#pragma mark - interposed IOKit

static CFTypeRef pb_IOHIDDeviceGetProperty(IOHIDDeviceRef dev, CFStringRef key) {
    if (key && pad_for(dev, 1)) {
        if (CFEqual(key, CFSTR(kIOHIDVendorIDKey))) return num_vid;
        if (CFEqual(key, CFSTR(kIOHIDProductIDKey))) return num_pid;
        if (CFEqual(key, CFSTR(kIOHIDVersionNumberKey))) return num_version;
        if (CFEqual(key, CFSTR(kIOHIDProductKey))) return str_product;
        if (CFEqual(key, CFSTR(kIOHIDManufacturerKey))) return str_manufacturer;
        if (CFEqual(key, CFSTR(kIOHIDReportDescriptorKey))) return data_descriptor;
        if (CFEqual(key, CFSTR(kIOHIDMaxInputReportSizeKey))) return num_in_len;
        if (CFEqual(key, CFSTR(kIOHIDMaxOutputReportSizeKey))) return num_out_len;
        if (CFEqual(key, CFSTR(kIOHIDMaxFeatureReportSizeKey))) return num_feat_len;
    }
    return IOHIDDeviceGetProperty(dev, key);
}
DYLD_INTERPOSE(pb_IOHIDDeviceGetProperty, IOHIDDeviceGetProperty);

static void pb_IOHIDDeviceScheduleWithRunLoop(IOHIDDeviceRef dev, CFRunLoopRef loop, CFStringRef mode) {
    IOHIDDeviceScheduleWithRunLoop(dev, loop, mode);
    pad *p = pad_for(dev, 1);
    if (p && !p->loop) {
        p->loop = (CFRunLoopRef)CFRetain(loop);
        p->loop_mode = mode ? CFStringCreateCopy(NULL, mode) : NULL;
        if (p->wine_cb) pump_start(p);
    }
}
DYLD_INTERPOSE(pb_IOHIDDeviceScheduleWithRunLoop, IOHIDDeviceScheduleWithRunLoop);

static void pb_IOHIDDeviceRegisterInputReportCallback(IOHIDDeviceRef dev, uint8_t *buf, CFIndex len,
                                                      IOHIDReportCallback cb, void *ctx) {
    pad *p = pad_for(dev, 1);
    if (!p) {
        IOHIDDeviceRegisterInputReportCallback(dev, buf, len, cb, ctx);
        return;
    }
    if (!cb) {
        pb_log("input closed");
        pump_stop(p);
        p->wine_cb = NULL;
        IOHIDDeviceRegisterInputReportCallback(dev, NULL, 0, NULL, NULL);
        return;
    }
    p->wine_cb = cb, p->wine_ctx = ctx, p->wine_buf = buf, p->wine_len = len;
    pb_log("input opened (buffer %ld)", (long)len);
    IOHIDDeviceRegisterInputReportCallback(dev, p->real_buf, sizeof p->real_buf, on_real_report, p);
    pump_start(p);
    rumble_set(p, 0, 0);
}
DYLD_INTERPOSE(pb_IOHIDDeviceRegisterInputReportCallback, IOHIDDeviceRegisterInputReportCallback);

static IOReturn pb_IOHIDDeviceSetReport(IOHIDDeviceRef dev, IOHIDReportType type, CFIndex id,
                                        const uint8_t *report, CFIndex len) {
    pad *p = pad_for(dev, 1);
    if (!p) return IOHIDDeviceSetReport(dev, type, id, report, len);
    if (type == kIOHIDReportTypeOutput && id == 0x02) handle_ds5_output(p, report, len);
    else pb_log("game set %s report %02lx (%ld bytes), accepted",
                type == kIOHIDReportTypeFeature ? "feature" : "output", (long)id, (long)len);
    return kIOReturnSuccess;
}
DYLD_INTERPOSE(pb_IOHIDDeviceSetReport, IOHIDDeviceSetReport);

static IOReturn pb_IOHIDDeviceGetReport(IOHIDDeviceRef dev, IOHIDReportType type, CFIndex id,
                                        uint8_t *report, CFIndex *len) {
    pad *p = pad_for(dev, 1);
    if (!p) return IOHIDDeviceGetReport(dev, type, id, report, len);
    if (type != kIOHIDReportTypeFeature || !len) return kIOReturnUnsupported;
    *len = ds5_feature(p, (uint8_t)id, report, *len);
    pb_log("game read feature %02lx (%ld bytes)", (long)id, (long)*len);
    return kIOReturnSuccess;
}
DYLD_INTERPOSE(pb_IOHIDDeviceGetReport, IOHIDDeviceGetReport);

static IOReturn pb_IOHIDDeviceClose(IOHIDDeviceRef dev, IOOptionBits options) {
    pad *p = pad_for(dev, 0);
    if (p) {
        pb_log("8BitDo %p closed", (void *)dev);
        pump_stop(p);
        p->wine_cb = NULL;
        pthread_mutex_lock(&rumble_mu);
        if (rumble_pad == p) rumble_pad = NULL;
        pthread_mutex_unlock(&rumble_mu);
        send_motors(dev, 0, 0);
    }
    return IOHIDDeviceClose(dev, options);
}
DYLD_INTERPOSE(pb_IOHIDDeviceClose, IOHIDDeviceClose);

#pragma mark - load

__attribute__((destructor)) static void pb_fini(void) {
    pthread_mutex_lock(&pads_mu);
    for (pad *p = pads; p; p = p->next)
        if (p->wanted && p->dev) send_motors(p->dev, 0, 0);
    pthread_mutex_unlock(&pads_mu);
}

__attribute__((constructor)) static void pb_init(void) {
    load_config();
    if (cfg.log) {
        const char *home = getenv("HOME");
        char path[1024];
        snprintf(path, sizeof path, "%s/Library/Logs/padbridge.log", home ? home : "/tmp");
        pb_log_fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    }
    num_vid = make_num(DS5_VID);
    num_pid = make_num(DS5_PID);
    num_version = make_num(DS5_VERSION);
    num_in_len = make_num(DS5_INPUT_LEN);
    num_out_len = make_num(DS5_OUTPUT_LEN);
    num_feat_len = make_num(DS5_FEATURE_LEN);
    data_descriptor = CFDataCreate(NULL, ds5_report_descriptor, sizeof ds5_report_descriptor);
    str_product = CFSTR("DualSense Wireless Controller");
    str_manufacturer = CFSTR("Sony Interactive Entertainment");
    // winebus's SDL would otherwise open the "DualSense" too and drive it with its
    // own PS5 code. Wine already ignores SDL's copy of a hidraw DualSense; this
    // keeps SDL from writing to it.
    setenv("SDL_JOYSTICK_HIDAPI_PS5", "0", 0);
    pb_log("padbridge %s loaded (%s)", PB_VERSION, cfg.enabled ? "enabled" : "disabled by config");
}
