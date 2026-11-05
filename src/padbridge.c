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
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ds5.h"

#define PB_VERSION "0.2.0"

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
    int keep_alive;      // keep the DualSense while the pad sleeps
} cfg = {1, 1, MAP_TOUCHPAD, MAP_CREATE, MAP_MUTE, 100, 180, 0, 30, 0, 1};

static int pb_log_fd = -1;
static long pb_log_tz;  // seconds east of UTC, fixed at load: no localtime_r per line
static pthread_mutex_t pb_log_mu = PTHREAD_MUTEX_INITIALIZER;

static void pb_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void pb_log(const char *fmt, ...) {
    if (!cfg.log || pb_log_fd < 0) return;
    char line[1024];
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    int day = (int)((ts.tv_sec + pb_log_tz) % 86400);
    int n = snprintf(line, sizeof line, "%02d:%02d:%02d.%03ld [%d %s] ", day / 3600, day / 60 % 60,
                     day % 60, ts.tv_nsec / 1000000, getpid(), getprogname());
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

// ~/Library/Logs/padbridge.log, moved to padbridge.log.1 once it passes 8 MB.
// Every Wine process of the game opens it at once; the lock lets one rotate it
// and the others follow to the new file.
static void log_open(void) {
    const char *home = getenv("HOME");
    char path[1024], old[1040];
    snprintf(path, sizeof path, "%s/Library/Logs/padbridge.log", home ? home : "/tmp");
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return;
    struct stat a, b;
    if (flock(fd, LOCK_EX) == 0 && fstat(fd, &a) == 0) {
        int moved = stat(path, &b) != 0 || a.st_ino != b.st_ino;
        if (!moved && a.st_size > 8 << 20) {
            snprintf(old, sizeof old, "%s.1", path);
            moved = rename(path, old) == 0;
        }
        if (moved) {
            int nfd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
            if (nfd >= 0) close(fd), fd = nfd;
        }
    }
    flock(fd, LOCK_UN);
    pb_log_fd = fd;
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
        else if (!strcmp(key, "keep_alive")) cfg.keep_alive = num != 0;
    }
    fclose(f);
}

static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

#pragma mark - devices

// Pads are never freed: Wine and the rumble thread may still hold one after its
// device is gone, and the retained ref keeps its address from being reused.
typedef struct hook hook;

typedef struct pad {
    IOHIDDeviceRef dev;            // the real 8BitDo, as Wine holds it (retained)
    _Atomic int dead;              // closed: nothing more goes to the device
    // The interface reports come from and motor reports go to: dev, or the one
    // the pad came back on after sleeping (retained, changed under rumble_mu).
    _Atomic(IOHIDDeviceRef) live;
    _Atomic int asleep;            // live is gone; Wine wasn't told
    IOHIDManagerRef mgr;           // the manager that gave dev to Wine
    hook *gone;                    // that manager's removal callback, while asleep
    // Wine's input subscription, set on Wine's start/stop thread, read on its bus thread
    _Atomic(IOHIDReportCallback) wine_cb;
    _Atomic(void *) wine_ctx;
    _Atomic(uint8_t *) wine_buf;
    _Atomic CFIndex wine_len;
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
    CFRunLoopTimerRef pump;        // loop and these under pump_mu
    CFRunLoopSourceRef pump_src;
    // rumble, set by the game: 0-255, and its motor power cut in 12.5% steps, 0-7
    uint8_t want_left, want_right, power_cut;
    struct pad *next;
} pad;

static pthread_mutex_t pads_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t pump_mu = PTHREAD_MUTEX_INITIALIZER;
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
    // The gamepad interface only: asleep, the receiver shows a vendor-page "IDLE"
    // interface with the same IDs instead.
    return prop_int(dev, CFSTR(kIOHIDVendorIDKey)) == 0x2dc8 &&
           prop_int(dev, CFSTR(kIOHIDProductIDKey)) == 0x3016 &&
           prop_int(dev, CFSTR(kIOHIDPrimaryUsagePageKey)) == 1 &&
           prop_int(dev, CFSTR(kIOHIDPrimaryUsageKey)) == 5;
}

static pad *find_pad(IOHIDDeviceRef dev) {
    pad *p = pads;
    while (p && p->dev != dev) p = p->next;
    return p;
}

// Logged by the processes that use IOKit HID (Wine's winedevice), not by every
// Wine process the library is inserted into.
static void say_loaded(void) {
    pb_log("padbridge %s loaded (%s)", PB_VERSION, cfg.enabled ? "enabled" : "disabled by config");
}

// The pad for dev, made on first sight. NULL when dev is not the 8BitDo.
// Other devices aren't remembered: refs come and go (SDL's enumeration makes and
// frees them), and a freed address can come back as the 8BitDo. Checking them
// again costs a property read or two, on calls that are rare for them.
static pad *pad_for(IOHIDDeviceRef dev, int create) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, say_loaded);
    if (!dev || !cfg.enabled) return NULL;
    pthread_mutex_lock(&pads_mu);
    pad *p = find_pad(dev);
    pthread_mutex_unlock(&pads_mu);
    if (p || !create || !is_8bitdo(dev)) return p;
    pthread_mutex_lock(&pads_mu);
    if (!(p = find_pad(dev)) && (p = calloc(1, sizeof *p))) {
        p->dev = p->live = (IOHIDDeviceRef)CFRetain(dev);
        p->started_ns = now_ns();
        p->next = pads;
        pads = p;
        pb_log("8BitDo %p found, presenting it as a DualSense", (void *)dev);
    }
    pthread_mutex_unlock(&pads_mu);
    return p;
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

// The 8BitDo's sticks rest at 127, a DualSense's at 128: stretch 0-127 to 0-128
// and squeeze 127-255 to 128-255, so the ends stay where they are.
static uint8_t stick[256];

static void stick_init(void) {
    for (int v = 0; v < 256; v++)
        stick[v] = v <= 127 ? (v * 128 + 63) / 127 : 128 + ((v - 127) * 127 + 64) / 128;
}

static void build_ds5(pad *p, uint8_t *r) {
    const uint8_t *in = p->in;
    memset(r, 0, DS5_INPUT_LEN);
    r[0] = 0x01;
    if (p->have_in) {
        r[DS5_IN_LX] = stick[in[4]];
        r[DS5_IN_LY] = stick[in[5]];
        r[DS5_IN_RX] = stick[in[6]];
        r[DS5_IN_RY] = stick[in[7]];
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
// would for a real DualSense. Wine unsubscribes from another thread, so take
// one consistent copy of the subscription first.
static void emit(pad *p, IOReturn result) {
    IOHIDReportCallback cb = atomic_load_explicit(&p->wine_cb, memory_order_acquire);
    void *ctx = atomic_load_explicit(&p->wine_ctx, memory_order_relaxed);
    uint8_t *buf = atomic_load_explicit(&p->wine_buf, memory_order_relaxed);
    CFIndex len = atomic_load_explicit(&p->wine_len, memory_order_relaxed);
    if (!cb || !buf || len < DS5_INPUT_LEN) return;
    build_ds5(p, buf);
    p->last_emit_ns = now_ns();
    cb(ctx, result, p->dev, kIOHIDReportTypeInput, 0x01, buf, DS5_INPUT_LEN);
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
    emit(p, result);
    // The next repeat is due a full period after this report, not sooner.
    pthread_mutex_lock(&pump_mu);
    if (p->pump) CFRunLoopTimerSetNextFireDate(p->pump, CFAbsoluteTimeGetCurrent() + 0.004);
    pthread_mutex_unlock(&pump_mu);
    log_input(p);
}

// A DualSense streams at 250 Hz whether or not anything changed, and PlayStation
// code can take silence for a disconnect. The 8BitDo goes quiet when idle, so
// repeat the last state between its reports.
//
// Wine's bus thread runs its run loop with "return after a source is handled"
// and only then hands queued reports on; a timer firing doesn't count, so a
// timer alone left the repeats queued for up to 10 s. The timer signals a source
// on the same run loop instead, and the source emits.
static void on_pump_perform(void *ctx) {
    pad *p = ctx;
    if (now_ns() - p->last_emit_ns >= 2000000ull) emit(p, kIOReturnSuccess);
}

// ctx is the source, retained by the timer, so this needs no lock.
static void on_pump(CFRunLoopTimerRef t, void *ctx) {
    (void)t;
    CFRunLoopSourceSignal((CFRunLoopSourceRef)ctx);
    CFRunLoopWakeUp(CFRunLoopGetCurrent());
}

// Started and stopped from Wine's start/stop thread as well as its bus thread.
static void pump_start(pad *p) {
    pthread_mutex_lock(&pump_mu);
    if (!p->pump && p->loop) {
        CFStringRef mode = p->loop_mode ? p->loop_mode : kCFRunLoopDefaultMode;
        CFRunLoopSourceContext sc = {.info = p, .perform = on_pump_perform};
        p->pump_src = CFRunLoopSourceCreate(NULL, 0, &sc);
        CFRunLoopAddSource(p->loop, p->pump_src, mode);
        CFRunLoopTimerContext tc = {0, (void *)p->pump_src, CFRetain, CFRelease, NULL};
        p->pump = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 0.004, 0.004, 0, 0, on_pump, &tc);
        CFRunLoopAddTimer(p->loop, p->pump, mode);
    }
    pthread_mutex_unlock(&pump_mu);
}

static void pump_stop(pad *p) {
    pthread_mutex_lock(&pump_mu);
    if (p->pump) {
        CFRunLoopTimerInvalidate(p->pump);
        CFRelease(p->pump);
        p->pump = NULL;
    }
    if (p->pump_src) {
        CFRunLoopSourceInvalidate(p->pump_src);
        CFRelease(p->pump_src);
        p->pump_src = NULL;
    }
    pthread_mutex_unlock(&pump_mu);
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

// Game level 0-255 to motor level 0-1, after the game's power cut, strength,
// ceiling and floor. The motors are felt from level 4 up, so low levels pass
// through as they are. Near full power they get harsh, so strong levels bend
// smoothly towards rumble_max (tanh: linear at the bottom, flat at the top)
// instead of clipping.
static double shape(uint8_t v, int cut) {
    if (!v) return 0;
    double x = v / 255.0 * (8 - cut) / 8.0 * cfg.strength / 100.0;
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
    uint64_t sent_at = 0, last = 0;
    int failures = 0, fading = 0;
    pthread_mutex_lock(&rumble_mu);
    for (;;) {
        pad *p = rumble_pad;
        int cut = p ? p->power_cut : 0;
        double want_l = p ? shape(cfg.swap_motors ? p->want_right : p->want_left, cut) : 0;
        double want_r = p ? shape(cfg.swap_motors ? p->want_left : p->want_right, cut) : 0;
        rumble_dirty = 0;
        IOHIDDeviceRef dev = p && !atomic_load(&p->asleep) ? p->live : NULL;
        if (dev) CFRetain(dev);
        pthread_mutex_unlock(&rumble_mu);

        // Rises land at once. Falls fade over release_ms: an ERM motor coasts
        // anyway, and a short fade keeps quick pulses from chattering. The fade
        // goes by the clock, not by wakes (game reports wake this too), and
        // starts from the moment the level falls.
        uint64_t now = now_ns();
        double dt = fading ? (double)(now - last) : 0;
        last = now;
        double step = cfg.release_ms > 0 ? dt / (cfg.release_ms * 1e6) : 1;
        cur_l = want_l >= cur_l ? want_l : fmax(want_l, cur_l - step);
        cur_r = want_r >= cur_r ? want_r : fmax(want_r, cur_r - step);
        uint8_t out_l = (uint8_t)lrint(cur_l * 255), out_r = (uint8_t)lrint(cur_r * 255);
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
        fading = fabs(cur_l - want_l) > 1e-6 || fabs(cur_r - want_r) > 1e-6;
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

// Hands the game's levels to the rumble thread. Called for every game report,
// so an unchanged state returns before waking anything. cut 0xff keeps the cut.
static void rumble_set(pad *p, uint8_t left, uint8_t right, uint8_t cut, uint8_t f0, uint8_t f1,
                       uint8_t f2) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, rumble_start);
    pthread_mutex_lock(&rumble_mu);
    if (cut > 7) cut = p->power_cut;
    if (rumble_pad == p && left == p->want_left && right == p->want_right && cut == p->power_cut) {
        pthread_mutex_unlock(&rumble_mu);
        return;
    }
    if (cut != p->power_cut) pb_log("game motor power %d%% (cut %d)", 100 - cut * 25 / 2, cut);
    if (cfg.log >= 2 && (left != p->want_left || right != p->want_right))
        pb_log("game output: flags %02x/%02x/%02x  left %3d right %3d", f0, f1, f2, left, right);
    p->want_left = left;
    p->want_right = right;
    p->power_cut = cut;
    rumble_pad = p;
    rumble_dirty = 1;
    pthread_cond_signal(&rumble_cv);
    pthread_mutex_unlock(&rumble_mu);
}

static void handle_ds5_output(pad *p, const uint8_t *r, CFIndex len) {
    if (len < 5 || r[0] != 0x02 || atomic_load(&p->dead)) return;
    uint8_t f0 = r[DS5_OUT_FLAGS0], f1 = r[DS5_OUT_FLAGS1];
    uint8_t f2 = len > DS5_OUT_FLAGS2 ? r[DS5_OUT_FLAGS2] : 0;
    pthread_mutex_lock(&rumble_mu);
    uint8_t left = p->want_left, right = p->want_right, cut = p->power_cut;
    pthread_mutex_unlock(&rumble_mu);
    // The motor bytes count only with a rumble emulation bit (SDL_hidapi_ps5.c).
    // 0x02 alone, as in the game's lightbar reports, picks rumble over audio
    // haptics without new levels: the motors keep going. Neither bit hands the
    // motors back to audio haptics, which this pad doesn't have: silence.
    if ((f0 & 0x01) || (f2 & 0x04)) left = r[DS5_OUT_RUMBLE_LEFT], right = r[DS5_OUT_RUMBLE_RIGHT];
    else if (!(f0 & 0x02)) left = right = 0;
    // The rumble power cut, valid with flags1 0x40, likely the game's vibration setting.
    if ((f1 & 0x40) && len > DS5_OUT_POWER) {
        cut = r[DS5_OUT_POWER] & 0x0f;
        if (cut > 7) cut = 7;
    }
    rumble_set(p, left, right, cut, f0, f1, f2);
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
    if (!p) return;
    pthread_mutex_lock(&pump_mu);
    int first = !p->loop;
    if (first) {
        p->loop = (CFRunLoopRef)CFRetain(loop);
        p->loop_mode = mode ? CFStringCreateCopy(NULL, mode) : NULL;
    }
    pthread_mutex_unlock(&pump_mu);
    if (first && atomic_load(&p->wine_cb)) pump_start(p);
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
        // Wine unsubscribes on removal and again when it stops the device.
        if (atomic_exchange(&p->wine_cb, NULL)) pb_log("input closed");
        pump_stop(p);
        IOHIDDeviceRegisterInputReportCallback(dev, NULL, 0, NULL, NULL);
        return;
    }
    atomic_store_explicit(&p->wine_ctx, ctx, memory_order_relaxed);
    atomic_store_explicit(&p->wine_buf, buf, memory_order_relaxed);
    atomic_store_explicit(&p->wine_len, len, memory_order_relaxed);
    atomic_store_explicit(&p->wine_cb, cb, memory_order_release);
    pb_log("input opened (buffer %ld)", (long)len);
    IOHIDDeviceRegisterInputReportCallback(dev, p->real_buf, sizeof p->real_buf, on_real_report, p);
    pump_start(p);
    rumble_set(p, 0, 0, 0xff, 0, 0, 0);
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
    if (p && !atomic_exchange(&p->dead, 1)) {
        pb_log("8BitDo %p closed", (void *)dev);
        atomic_store(&p->wine_cb, NULL);
        pump_stop(p);
        pthread_mutex_lock(&rumble_mu);
        if (rumble_pad == p) rumble_pad = NULL;
        pthread_mutex_unlock(&rumble_mu);
        if (!atomic_load(&p->asleep)) send_motors(p->live, 0, 0);
    }
    return IOHIDDeviceClose(dev, options);
}
DYLD_INTERPOSE(pb_IOHIDDeviceClose, IOHIDDeviceClose);

#pragma mark - sleep and wake

// After a few idle minutes the pad sleeps: the receiver drops its gamepad
// interface (and shows a vendor "IDLE" one), then brings a new gamepad interface
// back when a button wakes it. Wine would remove the DualSense and add another,
// which the game may not pick up. So the removal is kept from Wine: the DualSense
// stays, streaming a centred pad, and moves onto the new interface on wake.
//
// Both run on Wine's bus thread (its manager's run loop), as on_real_report does.

struct hook {
    IOHIDDeviceCallback cb;
    void *ctx;
    IOHIDManagerRef mgr;
};

// The pad Wine holds through mgr whose live interface is dev, or whichever is
// asleep there when dev is NULL.
static pad *bound_pad(IOHIDManagerRef mgr, IOHIDDeviceRef dev) {
    pthread_mutex_lock(&pads_mu);
    pad *p = pads;
    while (p && (p->mgr != mgr || atomic_load(&p->dead) || !p->loop ||
                 (dev ? p->live != dev || atomic_load(&p->asleep) : !atomic_load(&p->asleep))))
        p = p->next;
    pthread_mutex_unlock(&pads_mu);
    return p;
}

// Whether Wine holds another live 8BitDo through mgr: then this one really went.
static int other_pad(IOHIDManagerRef mgr, pad *self) {
    pthread_mutex_lock(&pads_mu);
    pad *p = pads;
    while (p && (p == self || p->mgr != mgr || atomic_load(&p->dead) || atomic_load(&p->asleep) ||
                 !atomic_load(&p->wine_cb)))
        p = p->next;
    pthread_mutex_unlock(&pads_mu);
    return p != NULL;
}

static void on_removal(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef dev) {
    hook *h = ctx;
    pad *p = bound_pad(h->mgr, dev);
    if (!p || !atomic_load(&p->wine_cb) || other_pad(h->mgr, p)) {
        h->cb(h->ctx, result, sender, dev);
        return;
    }
    IOHIDDeviceRegisterInputReportCallback(dev, NULL, 0, NULL, NULL);
    p->gone = h;
    p->have_in = 0;  // nothing held down while it's away
    atomic_store(&p->asleep, 1);
    rumble_set(p, 0, 0, 0xff, 0, 0, 0);
    pb_log("8BitDo %p gone (asleep?): keeping the DualSense", (void *)dev);
}

// Moves p onto dev. 0 when dev can't be opened.
static int rebind(pad *p, IOHIDDeviceRef dev) {
    if (IOHIDDeviceOpen(dev, 0) != kIOReturnSuccess) return 0;
    CFStringRef mode = p->loop_mode ? p->loop_mode : kCFRunLoopDefaultMode;
    IOHIDDeviceScheduleWithRunLoop(dev, p->loop, mode);
    IOHIDDeviceRegisterInputReportCallback(dev, p->real_buf, sizeof p->real_buf, on_real_report, p);
    pthread_mutex_lock(&rumble_mu);
    IOHIDDeviceRef old = p->live;
    p->live = (IOHIDDeviceRef)CFRetain(dev);
    atomic_store(&p->asleep, 0);
    pthread_mutex_unlock(&rumble_mu);
    // Wine's own ref stays: Wine still calls with it. One of ours goes.
    if (old != p->dev) {
        IOHIDDeviceUnscheduleFromRunLoop(old, p->loop, mode);
        IOHIDDeviceClose(old, 0);
        CFRelease(old);
    }
    return 1;
}

static void on_matching(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef dev) {
    hook *h = ctx;
    pad *p = is_8bitdo(dev) ? bound_pad(h->mgr, NULL) : NULL;
    if (p && rebind(p, dev)) {
        pb_log("8BitDo %p back, the DualSense carries on", (void *)dev);
        return;
    }
    if (p) {
        // Can't have it: let Wine remove the old DualSense and take the new pad.
        pb_log("8BitDo %p back but won't open: handing it to Wine", (void *)dev);
        p->gone->cb(p->gone->ctx, kIOReturnSuccess, sender, p->dev);
    }
    h->cb(h->ctx, result, sender, dev);
    // Remember which manager gave Wine the 8BitDo.
    pthread_mutex_lock(&pads_mu);
    if ((p = find_pad(dev)) && !p->mgr) p->mgr = h->mgr;
    pthread_mutex_unlock(&pads_mu);
}

// One hook per registration, never freed: callbacks may still be on their way.
static hook *make_hook(IOHIDManagerRef mgr, IOHIDDeviceCallback cb, void *ctx) {
    hook *h = calloc(1, sizeof *h);
    if (h) h->cb = cb, h->ctx = ctx, h->mgr = mgr;
    return h;
}

static void pb_IOHIDManagerRegisterDeviceMatchingCallback(IOHIDManagerRef mgr, IOHIDDeviceCallback cb,
                                                          void *ctx) {
    hook *h = cb && cfg.enabled && cfg.keep_alive ? make_hook(mgr, cb, ctx) : NULL;
    if (h) IOHIDManagerRegisterDeviceMatchingCallback(mgr, on_matching, h);
    else IOHIDManagerRegisterDeviceMatchingCallback(mgr, cb, ctx);
}
DYLD_INTERPOSE(pb_IOHIDManagerRegisterDeviceMatchingCallback, IOHIDManagerRegisterDeviceMatchingCallback);

static void pb_IOHIDManagerRegisterDeviceRemovalCallback(IOHIDManagerRef mgr, IOHIDDeviceCallback cb,
                                                         void *ctx) {
    hook *h = cb && cfg.enabled && cfg.keep_alive ? make_hook(mgr, cb, ctx) : NULL;
    if (h) IOHIDManagerRegisterDeviceRemovalCallback(mgr, on_removal, h);
    else IOHIDManagerRegisterDeviceRemovalCallback(mgr, cb, ctx);
}
DYLD_INTERPOSE(pb_IOHIDManagerRegisterDeviceRemovalCallback, IOHIDManagerRegisterDeviceRemovalCallback);

#pragma mark - load

__attribute__((destructor)) static void pb_fini(void) {
    pthread_mutex_lock(&pads_mu);
    for (pad *p = pads; p; p = p->next)
        if (!atomic_load(&p->dead) && !atomic_load(&p->asleep)) send_motors(p->live, 0, 0);
    pthread_mutex_unlock(&pads_mu);
}

__attribute__((constructor)) static void pb_init(void) {
    load_config();
    if (cfg.log) {
        time_t t = time(NULL);
        struct tm tm;
        if (localtime_r(&t, &tm)) pb_log_tz = tm.tm_gmtoff;
        log_open();
    }
    stick_init();
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
    if (cfg.log >= 2) pb_log("padbridge %s inserted", PB_VERSION);
}
